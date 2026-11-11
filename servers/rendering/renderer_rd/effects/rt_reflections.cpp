/**************************************************************************/
/*  rt_reflections.cpp                                                    */
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

#include "rt_reflections.h"

#include "servers/rendering/renderer_compositor.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/renderer_scene_render.h"
#include "servers/rendering/rendering_server_globals.h"

using namespace RendererRD;

RTReflections::RTReflections() {
	Vector<String> modes;
	modes.push_back("");
	String defines = "\n#define MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS " + itos(RendererSceneRender::MAX_DIRECTIONAL_LIGHTS) + "\n";
	shader.initialize(modes, defines);
	shader_version = shader.version_create();
	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, 0));
	params_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(Params));
	// Never empty, so the uniform set is always complete.
	instances_capacity = 64;
	instances_buffer = RD::get_singleton()->storage_buffer_create(instances_capacity * sizeof(Instance));
	materials_capacity = 64;
	materials_buffer = RD::get_singleton()->storage_buffer_create(materials_capacity * sizeof(Material));

	Vector<String> average_modes;
	average_modes.push_back("");
	average_shader.initialize(average_modes);
	average_shader_version = average_shader.version_create();
	average_pipeline = RD::get_singleton()->compute_pipeline_create(average_shader.version_get_shader(average_shader_version, 0));
	averages_capacity = 64;
	averages_buffer = RD::get_singleton()->storage_buffer_create(averages_capacity * sizeof(float) * 4);
}

RTReflections::~RTReflections() {
	RD::get_singleton()->free_rid(averages_buffer);
	average_shader.version_free(average_shader_version);
	RD::get_singleton()->free_rid(materials_buffer);
	RD::get_singleton()->free_rid(instances_buffer);
	RD::get_singleton()->free_rid(params_buffer);
	shader.version_free(shader_version);
}

uint32_t RTReflections::get_texture_average(RID p_texture, RID p_rd_texture) {
	if (p_texture.is_null() || p_rd_texture.is_null()) {
		return NO_TEXTURE_AVERAGE;
	}
	const uint64_t frame = RSG::rasterizer->get_frame_number();
	uint32_t slot;
	const uint32_t *existing = texture_average_slots.getptr(p_texture);
	if (existing) {
		slot = *existing;
	} else {
		if (free_average_slots.size()) {
			slot = free_average_slots[free_average_slots.size() - 1];
			free_average_slots.resize(free_average_slots.size() - 1);
		} else {
			slot = average_slots.size();
			average_slots.push_back(AverageSlot());
		}
		average_slots[slot] = AverageSlot();
		average_slots[slot].texture = p_texture;
		texture_average_slots.insert(p_texture, slot);
	}
	AverageSlot &s = average_slots[slot];
	s.last_used = frame;
	if (s.rd_texture != p_rd_texture) {
		s.rd_texture = p_rd_texture;
		s.ready = false;
	}
	if (!s.ready && !s.queued) {
		s.queued = true;
		average_queue.push_back(slot);
	}
	return s.ready ? slot : NO_TEXTURE_AVERAGE;
}

void RTReflections::_update_texture_averages() {
	RD *rd = RD::get_singleton();
	const uint64_t frame = RSG::rasterizer->get_frame_number();

	// Free the slots of textures no material has asked for in a while (also those of freed textures).
	const uint64_t prune_interval = 256;
	if (frame >= averages_pruned + prune_interval) {
		averages_pruned = frame;
		for (uint32_t i = 0; i < average_slots.size(); i++) {
			AverageSlot &s = average_slots[i];
			if (s.texture.is_valid() && s.last_used + prune_interval < frame) {
				texture_average_slots.erase(s.texture);
				s = AverageSlot();
				free_average_slots.push_back(i);
			}
		}
	}

	if (average_slots.size() > averages_capacity) {
		const uint32_t capacity = Math::next_power_of_2(average_slots.size());
		RID buffer = rd->storage_buffer_create(capacity * sizeof(float) * 4);
		rd->buffer_copy(averages_buffer, buffer, 0, 0, averages_capacity * sizeof(float) * 4);
		rd->free_rid(averages_buffer);
		averages_buffer = buffer;
		averages_capacity = capacity;
	}

	if (average_queue.is_empty()) {
		return;
	}

	// A few textures per frame, so that loading a level does not stall a frame.
	const uint32_t budget = 8;
	uint32_t done = 0;
	uint32_t taken = 0;
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	RID sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED);
	RID shader_rid = average_shader.version_get_shader(average_shader_version, 0);
	RD::ComputeListID compute_list = 0;
	bool started = false;
	while (taken < average_queue.size() && done < budget) {
		const uint32_t slot = average_queue[taken++];
		AverageSlot &s = average_slots[slot];
		if (!s.queued) {
			continue; // Freed since it was queued.
		}
		s.queued = false;
		if (!rd->texture_is_valid(s.rd_texture)) {
			continue;
		}
		const RD::TextureFormat format = rd->texture_get_format(s.rd_texture);
		if (format.texture_type != RD::TEXTURE_TYPE_2D) {
			continue;
		}
		// The first mip whose larger side is below 256 texels: the 64x64 grid of bilinear taps covers a 128 texel mip
		// exactly, and staying high in the chain keeps the mips (often averaged in sRGB space) from biasing the color.
		const uint32_t largest = MAX(format.width, format.height);
		float lod = 0.0;
		if (largest >= 256) {
			lod = Math::floor(Math::log2(float(largest) / 128.0f));
		}
		lod = CLAMP(lod, 0.0f, float(MAX(format.mipmaps, 1u) - 1));
		if (!started) {
			started = true;
			compute_list = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(compute_list, average_pipeline);
		}
		RD::Uniform u_texture(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, s.rd_texture }));
		RD::Uniform u_averages(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, averages_buffer);
		rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_texture, u_averages), 0);
		struct {
			uint32_t slot;
			float lod;
			uint32_t pad[2];
		} push = { slot, lod, { 0, 0 } };
		rd->compute_list_set_push_constant(compute_list, &push, sizeof(push));
		rd->compute_list_dispatch(compute_list, 1, 1, 1);
		s.ready = true;
		done++;
	}
	if (started) {
		rd->compute_list_end();
	}
	// Drop what was taken; the rest waits for the next frames.
	if (taken >= average_queue.size()) {
		average_queue.clear();
	} else {
		LocalVector<uint32_t> rest;
		for (uint32_t i = taken; i < average_queue.size(); i++) {
			rest.push_back(average_queue[i]);
		}
		average_queue = rest;
	}
}

void RTReflections::update_tables(const LocalVector<Instance> &p_instances, const LocalVector<Material> &p_materials) {
	if (p_instances.size() > instances_capacity) {
		RD::get_singleton()->free_rid(instances_buffer);
		instances_capacity = Math::next_power_of_2(p_instances.size());
		instances_buffer = RD::get_singleton()->storage_buffer_create(instances_capacity * sizeof(Instance));
	}
	if (p_materials.size() > materials_capacity) {
		RD::get_singleton()->free_rid(materials_buffer);
		materials_capacity = Math::next_power_of_2(p_materials.size());
		materials_buffer = RD::get_singleton()->storage_buffer_create(materials_capacity * sizeof(Material));
	}
	if (p_instances.size()) {
		RD::get_singleton()->buffer_update(instances_buffer, 0, p_instances.size() * sizeof(Instance), p_instances.ptr());
	}
	if (p_materials.size()) {
		RD::get_singleton()->buffer_update(materials_buffer, 0, p_materials.size() * sizeof(Material), p_materials.ptr());
	}
	_update_texture_averages();
}

void RTReflections::process(Ref<RenderSceneBuffersRD> p_render_buffers, RTDenoiser::History &r_history, RID p_tlas, RID p_depth, RID p_normal_roughness, RID p_last_frame, RID p_destination, const Lights &p_lights, const Projection &p_projection, const Transform3D &p_cam_transform, const Projection &p_prev_projection, const Transform3D &p_prev_cam_transform, uint64_t p_frame, const Settings &p_settings) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	MaterialStorage *material_storage = MaterialStorage::get_singleton();

	const Size2i size = p_render_buffers->get_internal_size();
	const uint32_t usage = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
	const bool denoise = p_settings.denoise && p_settings.debug_view == 0;
	RID raw = denoise ? p_render_buffers->create_texture(RB_SCOPE_RT_REFLECTIONS, SNAME("raw"), RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, size, 1) : p_destination;
	RID hit_distance = p_render_buffers->create_texture(RB_SCOPE_RT_REFLECTIONS, SNAME("hit_distance"), RD::DATA_FORMAT_R16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, size, 1);

	Projection correction;
	correction.set_depth_correction(true);
	const Projection projection = correction * p_projection;
	const Projection prev_projection = correction * p_prev_projection;
	const Transform3D world_to_view = p_cam_transform.affine_inverse();

	Params params = {};
	MaterialStorage::store_camera(projection.inverse(), params.inv_projection);
	MaterialStorage::store_camera(projection, params.projection);
	MaterialStorage::store_transform(p_cam_transform, params.view_to_world);
	MaterialStorage::store_transform(world_to_view, params.world_to_view);
	MaterialStorage::store_camera(prev_projection * Projection(p_prev_cam_transform.affine_inverse() * p_cam_transform), params.reprojection);
	Projection jittered_correction;
	jittered_correction.set_depth_correction(true);
	jittered_correction.add_jitter_offset(p_settings.taa_jitter);
	MaterialStorage::store_camera(jittered_correction * p_projection * Projection(world_to_view), params.world_to_clip);
	params.ambient[0] = p_settings.ambient.r;
	params.ambient[1] = p_settings.ambient.g;
	params.ambient[2] = p_settings.ambient.b;
	params.screen_size[0] = size.x;
	params.screen_size[1] = size.y;
	params.frame = uint32_t(p_frame);
	params.instance_mask = p_settings.instance_mask;
	params.omni_count = p_lights.omni_buffer.is_valid() ? p_lights.omni_count : 0;
	params.spot_count = p_lights.spot_buffer.is_valid() ? p_lights.spot_count : 0;
	params.directional_count = p_lights.directional_buffer.is_valid() ? p_lights.directional_count : 0;
	params.shadowed_lights = MIN(p_settings.shadowed_lights, 4u);
	params.max_roughness = p_settings.max_roughness;
	params.normal_bias = 0.01;
	params.emission_scale = p_settings.emission_scale;
	params.debug_view = p_settings.debug_view;
	params.screen_reuse = p_settings.screen_reuse ? 1 : 0;
	params.view_consistent = p_settings.view_consistent_hits ? 1 : 0;
	RD::get_singleton()->buffer_update(params_buffer, 0, sizeof(Params), &params);

	RID nearest = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID linear = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID compute_shader = shader.version_get_shader(shader_version, 0);

	// Lights without data this frame bind a table that is never read (their count is zero).
	RID omni = p_lights.omni_buffer.is_valid() ? p_lights.omni_buffer : instances_buffer;
	RID spot = p_lights.spot_buffer.is_valid() ? p_lights.spot_buffer : instances_buffer;

	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest, p_depth }));
	RD::Uniform u_normal(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest, p_normal_roughness }));
	RD::Uniform u_tlas(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 2, p_tlas);
	RD::Uniform u_instances(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, instances_buffer);
	RD::Uniform u_materials(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, materials_buffer);
	RD::Uniform u_omni(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, omni);
	RD::Uniform u_spot(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, spot);
	RD::Uniform u_directional(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 7, p_lights.directional_buffer);
	RD::Uniform u_last_frame(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 8, Vector<RID>({ linear, p_last_frame }));
	RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 9, raw);
	RD::Uniform u_hit(RD::UNIFORM_TYPE_IMAGE, 10, hit_distance);
	RD::Uniform u_params(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 11, params_buffer);
	RD::Uniform u_averages(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, averages_buffer);

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(compute_shader, 0, u_depth, u_normal, u_tlas, u_instances, u_materials, u_omni, u_spot, u_directional, u_last_frame, u_dest, u_hit, u_params, u_averages), 0);
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);
	RD::get_singleton()->compute_list_end();

	if (denoise) {
		RENDER_TIMESTAMP("Denoise Ray Traced Reflections");
		RD::get_singleton()->draw_command_begin_label("Denoise Ray Traced Reflections");
		RTDenoiser::Settings denoise_settings;
		denoise_settings.max_history = 16.0;
		denoise_settings.max_history_dynamic = 4.0;
		denoiser.denoise_color(p_render_buffers, RB_SCOPE_RT_REFLECTIONS_DENOISE, r_history, raw, hit_distance, p_destination, p_depth, p_normal_roughness, p_projection, p_cam_transform, p_frame, denoise_settings);
		RD::get_singleton()->draw_command_end_label();
	} else {
		p_render_buffers->clear_context(RB_SCOPE_RT_REFLECTIONS_DENOISE);
		r_history = RTDenoiser::History();
	}
}
