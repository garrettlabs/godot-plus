/**************************************************************************/
/*  rt_denoiser.cpp                                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "rt_denoiser.h"

#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

RTDenoiser::RTDenoiser() {
	Vector<String> modes;
	modes.push_back("\n#define MODE_TEMPORAL\n");
	modes.push_back("\n#define MODE_SPATIAL\n");
	shader.initialize(modes);
	shader_version = shader.version_create();
	for (int i = 0; i < MODE_MAX; i++) {
		pipelines[i] = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, i));
	}
	params_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(Params));
}

RTDenoiser::~RTDenoiser() {
	RD::get_singleton()->free_rid(params_buffer);
	shader.version_free(shader_version);
}

RID RTDenoiser::denoise(Ref<RenderSceneBuffersRD> p_render_buffers, const StringName &p_context, History &r_history, RID p_signal, RID p_depth, RID p_normal_roughness, const Projection &p_projection, const Transform3D &p_cam_transform, uint64_t p_frame, const Settings &p_settings) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	MaterialStorage *material_storage = MaterialStorage::get_singleton();

	static const StringName history_names[2] = { SNAME("history_0"), SNAME("history_1") };
	static const StringName guide_names[2] = { SNAME("guide_0"), SNAME("guide_1") };
	static const StringName ping_names[2] = { SNAME("spatial_0"), SNAME("spatial_1") };

	const Size2i size = p_render_buffers->get_internal_size();
	const uint32_t usage = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;

	// The render buffers drop every texture on resize; history is also stale after a skipped frame (the effect was
	// off or the view was not drawn).
	bool history_valid = p_render_buffers->has_texture(p_context, history_names[0]) && r_history.size == size && r_history.last_frame != UINT64_MAX && r_history.last_frame + 1 == p_frame;
	if (r_history.size != size) {
		p_render_buffers->clear_context(p_context);
	}
	for (int i = 0; i < 2; i++) {
		p_render_buffers->create_texture(p_context, history_names[i], RD::DATA_FORMAT_R16G16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, size, 1);
		p_render_buffers->create_texture(p_context, guide_names[i], RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, size, 1);
		p_render_buffers->create_texture(p_context, ping_names[i], RD::DATA_FORMAT_R16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, size, 1);
	}

	const uint32_t current = history_valid ? 1 - r_history.index : 0;
	const uint32_t previous = 1 - current;

	Projection correction;
	correction.set_depth_correction(true);
	const Projection projection = correction * p_projection;

	Params params = {};
	MaterialStorage::store_camera(projection.inverse(), params.inv_projection);
	MaterialStorage::store_transform(p_cam_transform, params.view_to_world);
	MaterialStorage::store_transform(r_history.prev_cam_transform.affine_inverse() * p_cam_transform, params.reprojection);
	MaterialStorage::store_camera(r_history.prev_projection, params.prev_projection);
	params.screen_size[0] = size.x;
	params.screen_size[1] = size.y;
	params.history_valid = history_valid ? 1 : 0;
	params.max_history = MAX(1.0f, p_settings.max_history);
	params.max_history_dynamic = MAX(1.0f, p_settings.max_history_dynamic);
	params.clamp_sigma = p_settings.clamp_sigma;
	RD::get_singleton()->buffer_update(params_buffer, 0, sizeof(Params), &params);

	RID nearest = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID history_cur = p_render_buffers->get_texture(p_context, history_names[current]);
	RID history_prev = p_render_buffers->get_texture(p_context, history_names[previous]);
	RID guide_cur = p_render_buffers->get_texture(p_context, guide_names[current]);
	RID guide_prev = p_render_buffers->get_texture(p_context, guide_names[previous]);
	RID ping[2] = { p_render_buffers->get_texture(p_context, ping_names[0]), p_render_buffers->get_texture(p_context, ping_names[1]) };

	RD::Uniform u_params(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, params_buffer);
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest, p_depth }));
	RD::Uniform u_normal(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest, p_normal_roughness }));

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();

	// Temporal accumulation.
	{
		RID shader_rid = shader.version_get_shader(shader_version, MODE_TEMPORAL);
		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_TEMPORAL]);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_params), 0);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 1, u_depth, u_normal), 1);
		RD::Uniform u_signal(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest, p_signal }));
		RD::Uniform u_history(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest, history_prev }));
		RD::Uniform u_guide(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest, guide_prev }));
		RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 3, history_cur);
		RD::Uniform u_dest_guide(RD::UNIFORM_TYPE_IMAGE, 4, guide_cur);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 2, u_signal, u_history, u_guide, u_dest, u_dest_guide), 2);
		PushConstant push = {};
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &push, sizeof(PushConstant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);
		RD::get_singleton()->compute_list_add_barrier(compute_list);
	}

	// Spatial filtering: steps 1, 2, 4... The first iteration fades out above 16 frames of history, the later
	// (wider) ones sooner, so a converged pixel keeps its detail and a fresh one gets the widest filter.
	RID result = history_cur;
	{
		RID shader_rid = shader.version_get_shader(shader_version, MODE_SPATIAL);
		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_SPATIAL]);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_params), 0);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 1, u_depth, u_normal), 1);
		RD::Uniform u_history(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest, history_cur }));
		const uint32_t iterations = CLAMP(p_settings.spatial_iterations, 0u, 5u);
		for (uint32_t i = 0; i < iterations; i++) {
			RID dest = ping[i & 1];
			RD::Uniform u_signal(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest, result }));
			RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 2, dest);
			RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 2, u_signal, u_history, u_dest), 2);
			PushConstant push = {};
			push.step = 1 << i;
			push.fade_end = i == 0 ? 24.0f : (i == 1 ? 12.0f : 6.0f);
			push.fade_start = push.fade_end * 0.5f;
			RD::get_singleton()->compute_list_set_push_constant(compute_list, &push, sizeof(PushConstant));
			RD::get_singleton()->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);
			RD::get_singleton()->compute_list_add_barrier(compute_list);
			result = dest;
		}
	}

	RD::get_singleton()->compute_list_end();

	r_history.prev_projection = projection;
	r_history.prev_cam_transform = p_cam_transform;
	r_history.last_frame = p_frame;
	r_history.index = current;
	r_history.size = size;

	return result;
}
