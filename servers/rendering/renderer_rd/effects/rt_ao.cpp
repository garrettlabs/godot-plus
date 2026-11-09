/**************************************************************************/
/*  rt_ao.cpp                                                             */
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

#include "rt_ao.h"

#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"

using namespace RendererRD;

RTAmbientOcclusion::RTAmbientOcclusion() {
	Vector<String> modes;
	modes.push_back("\n#define MODE_TRACE\n");
	modes.push_back("\n#define MODE_RESOLVE\n");
	shader.initialize(modes);
	shader_version = shader.version_create();
	for (int i = 0; i < MODE_MAX; i++) {
		pipelines[i] = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, i));
	}
	params_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(Params));
}

RTAmbientOcclusion::~RTAmbientOcclusion() {
	RD::get_singleton()->free_rid(params_buffer);
	shader.version_free(shader_version);
}

void RTAmbientOcclusion::process(Ref<RenderSceneBuffersRD> p_render_buffers, RTDenoiser::History &r_history, RID p_tlas, RID p_depth, RID p_normal_roughness, RID p_destination, const Projection &p_projection, const Transform3D &p_cam_transform, uint64_t p_frame, const Settings &p_settings) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	MaterialStorage *material_storage = MaterialStorage::get_singleton();

	const Size2i size = p_render_buffers->get_internal_size();
	RID visibility = p_render_buffers->create_texture(RB_SCOPE_RT_AO, SNAME("visibility"), RD::DATA_FORMAT_R16_SFLOAT, RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, size, 1);

	Params params = {};
	// Same depth convention as the screen-space effects: reverse Z, Y flipped.
	Projection correction;
	correction.set_depth_correction(true);
	MaterialStorage::store_camera((correction * p_projection).inverse(), params.inv_projection);
	MaterialStorage::store_transform(p_cam_transform, params.view_to_world);
	params.screen_size[0] = size.x;
	params.screen_size[1] = size.y;
	params.frame = uint32_t(p_frame);
	params.ray_count = CLAMP(p_settings.ray_count, 1u, 8u);
	params.radius = MAX(0.01f, p_settings.radius);
	params.intensity = p_settings.intensity * INTENSITY_SCALE;
	params.power = p_settings.power;
	params.instance_mask = p_settings.instance_mask;
	params.normal_bias = 0.01;
	RD::get_singleton()->buffer_update(params_buffer, 0, sizeof(Params), &params);

	RID nearest = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RD::Uniform u_params(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 5, params_buffer);

	{
		RID shader_rid = shader.version_get_shader(shader_version, MODE_TRACE);
		RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest, p_depth }));
		RD::Uniform u_normal(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest, p_normal_roughness }));
		RD::Uniform u_tlas(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 2, p_tlas);
		RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 3, visibility);

		RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_TRACE]);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_depth, u_normal, u_tlas, u_dest, u_params), 0);
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);
		RD::get_singleton()->compute_list_end();
	}

	RID filtered = visibility;
	if (p_settings.denoise) {
		RENDER_TIMESTAMP("Denoise Ray Traced AO");
		RD::get_singleton()->draw_command_begin_label("Denoise Ray Traced AO");
		filtered = denoiser.denoise(p_render_buffers, RB_SCOPE_RT_AO_DENOISE, r_history, visibility, p_depth, p_normal_roughness, p_projection, p_cam_transform, p_frame, RTDenoiser::Settings());
		RD::get_singleton()->draw_command_end_label();
	} else {
		// Nothing to carry over when the filter comes back on.
		p_render_buffers->clear_context(RB_SCOPE_RT_AO_DENOISE);
		r_history = RTDenoiser::History();
	}

	{
		RID shader_rid = shader.version_get_shader(shader_version, MODE_RESOLVE);
		RD::Uniform u_source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest, filtered }));
		RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 1, p_destination);

		RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_RESOLVE]);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_source, u_dest, u_params), 0);
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);
		RD::get_singleton()->compute_list_end();
	}
}
