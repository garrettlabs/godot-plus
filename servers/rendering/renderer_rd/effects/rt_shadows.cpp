/**************************************************************************/
/*  rt_shadows.cpp                                                        */
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

#include "rt_shadows.h"

#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

RTShadows::RTShadows() {
	Vector<String> modes;
	modes.push_back("");
	shader.initialize(modes);
	shader_version = shader.version_create();
	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, 0));
	params_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(Params));
}

RTShadows::~RTShadows() {
	if (lights_buffer.is_valid()) {
		RD::get_singleton()->free_rid(lights_buffer);
	}
	RD::get_singleton()->free_rid(params_buffer);
	shader.version_free(shader_version);
}

void RTShadows::trace(RID p_tlas, RID p_depth, RID p_normal_roughness, RID p_visibility_buffer, const Size2i &p_size, const Projection &p_projection, const Transform3D &p_cam_transform, const LocalVector<Light> &p_lights, uint32_t p_instance_mask, uint32_t p_soft_samples, uint32_t p_frame) {
	ERR_FAIL_COND(p_lights.is_empty());
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	MaterialStorage *material_storage = MaterialStorage::get_singleton();

	if (p_lights.size() > lights_capacity) {
		if (lights_buffer.is_valid()) {
			RD::get_singleton()->free_rid(lights_buffer);
		}
		lights_capacity = MAX(16u, Math::next_power_of_2(p_lights.size()));
		lights_buffer = RD::get_singleton()->storage_buffer_create(lights_capacity * sizeof(Light));
	}
	RD::get_singleton()->buffer_update(lights_buffer, 0, p_lights.size() * sizeof(Light), p_lights.ptr());

	Params params = {};
	// Same depth convention as the screen-space effects: reverse Z, Y flipped.
	Projection correction;
	correction.set_depth_correction(true);
	MaterialStorage::store_camera((correction * p_projection).inverse(), params.inv_projection);
	MaterialStorage::store_transform(p_cam_transform, params.view_to_world);
	params.screen_size[0] = p_size.x;
	params.screen_size[1] = p_size.y;
	params.light_count = p_lights.size();
	params.frame = p_frame;
	params.soft_samples = MAX(1u, p_soft_samples);
	params.instance_mask = p_instance_mask;
	params.normal_bias = 0.01;
	RD::get_singleton()->buffer_update(params_buffer, 0, sizeof(Params), &params);

	RID sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID compute_shader = shader.version_get_shader(shader_version, 0);

	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, p_depth }));
	RD::Uniform u_normal(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, p_normal_roughness }));
	RD::Uniform u_tlas(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 2, p_tlas);
	RD::Uniform u_lights(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, lights_buffer);
	RD::Uniform u_visibility(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, p_visibility_buffer);
	RD::Uniform u_params(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 5, params_buffer);

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(compute_shader, 0, u_depth, u_normal, u_tlas, u_lights, u_visibility, u_params), 0);
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_size.x, p_size.y, 1);
	RD::get_singleton()->compute_list_end();
}
