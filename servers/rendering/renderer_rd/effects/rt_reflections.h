/**************************************************************************/
/*  rt_reflections.h                                                      */
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

#pragma once

#include "core/math/projection.h"
#include "servers/rendering/renderer_rd/effects/rt_denoiser.h"
#include "servers/rendering/renderer_rd/shaders/effects/rt_reflections.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/rendering_device.h"

#define RB_SCOPE_RT_REFLECTIONS SNAME("rb_rt_reflections")
#define RB_SCOPE_RT_REFLECTIONS_DENOISE SNAME("rb_rt_reflections_denoise")

namespace RendererRD {

// Ray traced reflections: a compute pass with ray queries against the view's TLAS after the depth pre-pass. Each hit
// is taken from last frame's lit image where the depth buffer shows that surface, and otherwise shaded from a
// per-instance material record and the view's lights. The result is denoised and replaces what screen-space
// reflections would give the forward pass.
class RTReflections {
public:
	// One record per TLAS instance (indexed by the instance's custom index), mirrored by the shader.
	struct Instance {
		uint32_t vertex_address[2];
		uint32_t index_address[2]; // Zero for non-indexed geometry.
		uint32_t material;
		uint32_t flags;
		uint32_t pad[2];
	};
	static_assert(sizeof(Instance) == 32);

	enum InstanceFlags {
		INSTANCE_FLAG_PACKED = 1, // Positions R16G16B16A16_UNORM, else R32G32B32_SFLOAT.
		INSTANCE_FLAG_INDEX_16 = 2,
		INSTANCE_FLAG_GEOMETRY = 4, // The addresses are valid.
	};

	struct Material {
		float albedo_metallic[4]; // Linear albedo, metallic.
		float emission_roughness[4]; // Linear emission times energy, roughness.
	};
	static_assert(sizeof(Material) == 32);

	struct Lights {
		RID omni_buffer;
		RID spot_buffer;
		RID directional_buffer;
		uint32_t omni_count = 0;
		uint32_t spot_count = 0;
		uint32_t directional_count = 0;
	};

	struct Settings {
		uint32_t instance_mask = 0xFF;
		uint32_t shadowed_lights = 2;
		bool denoise = true;
		bool screen_reuse = true;
		uint32_t debug_view = 0;
		float max_roughness = 0.7; // The forward pass fades reflections out above 0.6 and drops them at 0.7.
		float emission_scale = 1.0;
		Color ambient;
	};

private:
	struct Params {
		float inv_projection[16];
		float projection[16];
		float view_to_world[16];
		float world_to_view[16];
		float reprojection[16];
		float ambient[4];
		int32_t screen_size[2];
		uint32_t frame;
		uint32_t instance_mask;
		uint32_t omni_count;
		uint32_t spot_count;
		uint32_t directional_count;
		uint32_t shadowed_lights;
		float max_roughness;
		float normal_bias;
		float emission_scale;
		uint32_t debug_view;
		uint32_t screen_reuse;
		uint32_t pad[3];
	};
	static_assert(sizeof(Params) % 16 == 0);

	RtReflectionsShaderRD shader;
	RID shader_version;
	RID pipeline;
	RID params_buffer;
	RID instances_buffer;
	uint32_t instances_capacity = 0;
	RID materials_buffer;
	uint32_t materials_capacity = 0;

	RTDenoiser denoiser;

public:
	RTReflections();
	~RTReflections();

	// Uploads this frame's instance and material records (shared by every view drawn this frame).
	void update_tables(const LocalVector<Instance> &p_instances, const LocalVector<Material> &p_materials);

	// Traces and denoises into p_destination (RGBA16F, storage, the internal size). p_prev_projection and
	// p_prev_cam_transform describe the frame p_last_frame was rendered with.
	void process(Ref<RenderSceneBuffersRD> p_render_buffers, RTDenoiser::History &r_history, RID p_tlas, RID p_depth, RID p_normal_roughness, RID p_last_frame, RID p_destination, const Lights &p_lights, const Projection &p_projection, const Transform3D &p_cam_transform, const Projection &p_prev_projection, const Transform3D &p_prev_cam_transform, uint64_t p_frame, const Settings &p_settings);
};

} // namespace RendererRD
