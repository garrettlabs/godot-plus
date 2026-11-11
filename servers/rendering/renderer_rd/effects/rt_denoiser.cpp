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
	modes.push_back("\n#define MODE_TEMPORAL\n#define SIGNAL_COLOR\n");
	modes.push_back("\n#define MODE_SPATIAL\n#define SIGNAL_COLOR\n");
	modes.push_back("\n#define MODE_UPSAMPLE\n#define SIGNAL_COLOR\n");
	shader.initialize(modes);
	shader_version = shader.version_create();
	for (int i = 0; i < MODE_MAX; i++) {
		pipelines[i] = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, i));
	}
	params_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(Params));
	params_buffer_color = RD::get_singleton()->uniform_buffer_create(sizeof(Params));
}

RTDenoiser::~RTDenoiser() {
	RD::get_singleton()->free_rid(params_buffer);
	RD::get_singleton()->free_rid(params_buffer_color);
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

void RTDenoiser::_update_params(RID p_buffer, const Size2i &p_size, bool p_history_valid, const Projection &p_projection, const Transform3D &p_cam_transform, const History &p_history, const Settings &p_settings) {
	Params params = {};
	MaterialStorage::store_camera(p_projection.inverse(), params.inv_projection);
	MaterialStorage::store_transform(p_cam_transform, params.view_to_world);
	MaterialStorage::store_transform(p_history.prev_cam_transform.affine_inverse() * p_cam_transform, params.reprojection);
	MaterialStorage::store_camera(p_history.prev_projection, params.prev_projection);
	MaterialStorage::store_camera(p_history.prev_projection.inverse(), params.prev_inv_projection);
	params.screen_size[0] = p_size.x;
	params.screen_size[1] = p_size.y;
	params.history_valid = p_history_valid ? 1 : 0;
	params.max_history = MAX(1.0f, p_settings.max_history);
	params.max_history_dynamic = MAX(1.0f, p_settings.max_history_dynamic);
	params.clamp_sigma = p_settings.clamp_sigma;
	RD::get_singleton()->buffer_update(p_buffer, 0, sizeof(Params), &params);
}

void RTDenoiser::denoise_color(Ref<RenderSceneBuffersRD> p_render_buffers, const StringName &p_context, History &r_history, RID p_signal, RID p_hit_distance, RID p_destination, RID p_depth, RID p_normal_roughness, const Size2i &p_size, const Projection &p_projection, const Transform3D &p_cam_transform, uint64_t p_frame, const Settings &p_settings) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	MaterialStorage *material_storage = MaterialStorage::get_singleton();

	static const StringName history_names[2] = { SNAME("history_0"), SNAME("history_1") };
	static const StringName length_names[2] = { SNAME("length_0"), SNAME("length_1") };
	static const StringName guide_names[2] = { SNAME("guide_0"), SNAME("guide_1") };
	static const StringName ping_names[2] = { SNAME("spatial_0"), SNAME("spatial_1") };

	const Size2i size = p_size;
	const uint32_t usage = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;

	bool history_valid = p_render_buffers->has_texture(p_context, history_names[0]) && r_history.size == size && r_history.last_frame != UINT64_MAX && r_history.last_frame + 1 == p_frame;
	if (r_history.size != size) {
		p_render_buffers->clear_context(p_context);
	}
	for (int i = 0; i < 2; i++) {
		p_render_buffers->create_texture(p_context, history_names[i], RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, size, 1);
		p_render_buffers->create_texture(p_context, length_names[i], RD::DATA_FORMAT_R16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, size, 1);
		p_render_buffers->create_texture(p_context, guide_names[i], RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, size, 1);
		p_render_buffers->create_texture(p_context, ping_names[i], RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, size, 1);
	}

	const uint32_t current = history_valid ? 1 - r_history.index : 0;
	const uint32_t previous = 1 - current;

	Projection correction;
	correction.set_depth_correction(true);
	// A half-size signal's pixel p was traced at full-size pixel 2p: map the projection so that the shaders' pixel
	// center (p + 0.5) / size lands there (ndc_full = a * ndc + b on each axis), keeping reconstruction and
	// reprojection exact.
	Projection grid;
	const Size2i full_size = p_render_buffers->get_internal_size();
	if (size != full_size) {
		for (int axis = 0; axis < 2; axis++) {
			const float a = 2.0f * size[axis] / float(full_size[axis]);
			const float b = a - 1.0f / float(full_size[axis]) - 1.0f;
			grid.columns[axis][axis] = 1.0f / a;
			grid.columns[3][axis] = -b / a;
		}
	}
	const Projection projection = grid * correction * p_projection;
	_update_params(params_buffer_color, size, history_valid, projection, p_cam_transform, r_history, p_settings);

	RID nearest = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID history_cur = p_render_buffers->get_texture(p_context, history_names[current]);
	RID history_prev = p_render_buffers->get_texture(p_context, history_names[previous]);
	RID length_cur = p_render_buffers->get_texture(p_context, length_names[current]);
	RID length_prev = p_render_buffers->get_texture(p_context, length_names[previous]);
	RID guide_cur = p_render_buffers->get_texture(p_context, guide_names[current]);
	RID guide_prev = p_render_buffers->get_texture(p_context, guide_names[previous]);
	RID ping[2] = { p_render_buffers->get_texture(p_context, ping_names[0]), p_render_buffers->get_texture(p_context, ping_names[1]) };

	RD::Uniform u_params(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, params_buffer_color);
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest, p_depth }));
	RD::Uniform u_normal(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest, p_normal_roughness }));

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();

	{
		RID shader_rid = shader.version_get_shader(shader_version, MODE_TEMPORAL_COLOR);
		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_TEMPORAL_COLOR]);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_params), 0);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 1, u_depth, u_normal), 1);
		RD::Uniform u_signal(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest, p_signal }));
		RD::Uniform u_history(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest, history_prev }));
		RD::Uniform u_guide(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest, guide_prev }));
		RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 3, history_cur);
		RD::Uniform u_dest_guide(RD::UNIFORM_TYPE_IMAGE, 4, guide_cur);
		RD::Uniform u_length(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ nearest, length_prev }));
		RD::Uniform u_dest_length(RD::UNIFORM_TYPE_IMAGE, 6, length_cur);
		RD::Uniform u_hit_distance(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ nearest, p_hit_distance }));
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 2, u_signal, u_history, u_guide, u_dest, u_dest_guide, u_length, u_dest_length, u_hit_distance), 2);
		PushConstant push = {};
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &push, sizeof(PushConstant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);
		RD::get_singleton()->compute_list_add_barrier(compute_list);
	}

	// Spatial filtering as for the scalar signal; the last iteration writes the destination.
	{
		RID shader_rid = shader.version_get_shader(shader_version, MODE_SPATIAL_COLOR);
		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_SPATIAL_COLOR]);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_params), 0);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 1, u_depth, u_normal), 1);
		RD::Uniform u_length(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest, length_cur }));
		const uint32_t iterations = CLAMP(p_settings.spatial_iterations, 1u, 5u);
		RID source = history_cur;
		for (uint32_t i = 0; i < iterations; i++) {
			RID dest = i + 1 == iterations ? p_destination : ping[i & 1];
			RD::Uniform u_signal(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest, source }));
			RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 2, dest);
			RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 2, u_signal, u_length, u_dest), 2);
			PushConstant push = {};
			push.step = 1 << i;
			push.fade_end = i == 0 ? 24.0f : (i == 1 ? 12.0f : 6.0f);
			push.fade_start = push.fade_end * 0.5f;
			RD::get_singleton()->compute_list_set_push_constant(compute_list, &push, sizeof(PushConstant));
			RD::get_singleton()->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);
			RD::get_singleton()->compute_list_add_barrier(compute_list);
			source = dest;
		}
	}

	RD::get_singleton()->compute_list_end();

	r_history.prev_projection = projection;
	r_history.prev_cam_transform = p_cam_transform;
	r_history.last_frame = p_frame;
	r_history.index = current;
	r_history.size = size;
}

void RTDenoiser::upsample_color(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_signal, RID p_guide_depth, RID p_guide_normal_roughness, RID p_destination, RID p_depth, RID p_normal_roughness, const Projection &p_projection) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	RID nearest = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	const Size2i full_size = p_render_buffers->get_internal_size();
	const RD::TextureFormat half_format = RD::get_singleton()->texture_get_format(p_signal);

	Projection correction;
	correction.set_depth_correction(true);
	const Projection inv_projection = (correction * p_projection).inverse();
	UpsamplePushConstant push = {};
	push.full_size[0] = full_size.x;
	push.full_size[1] = full_size.y;
	push.half_size[0] = half_format.width;
	push.half_size[1] = half_format.height;
	// View z and w of the inverse projection at the screen center: z = c22 * d + c32, w = c23 * d + c33.
	push.depth_to_linear[0] = inv_projection.columns[2][2];
	push.depth_to_linear[1] = inv_projection.columns[3][2];
	push.depth_to_linear[2] = inv_projection.columns[2][3];
	push.depth_to_linear[3] = inv_projection.columns[3][3];

	RID shader_rid = shader.version_get_shader(shader_version, MODE_UPSAMPLE_COLOR);
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest, p_depth }));
	RD::Uniform u_normal(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest, p_normal_roughness }));
	RD::Uniform u_signal(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest, p_signal }));
	RD::Uniform u_guide_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest, p_guide_depth }));
	RD::Uniform u_guide_normal(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest, p_guide_normal_roughness }));
	RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 3, p_destination);

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_UPSAMPLE_COLOR]);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 1, u_depth, u_normal), 1);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 2, u_signal, u_guide_depth, u_guide_normal, u_dest), 2);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push, sizeof(UpsamplePushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, full_size.x, full_size.y, 1);
	RD::get_singleton()->compute_list_end();
}
