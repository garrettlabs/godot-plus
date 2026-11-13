/**************************************************************************/
/*  rt_shadows.h                                                          */
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
#include "servers/rendering/renderer_rd/shaders/effects/rt_shadows.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

// Ray traced shadows for omni and spot lights: one compute pass with ray queries against the view's TLAS,
// run after the depth pre-pass. It writes, per pixel, up to six (light slot, visibility) entries plus the
// depth it traced from, which the forward pass reads instead of sampling shadow maps.
class RTShadows {
public:
	// Mirrors the shader's Light struct.
	struct Light {
		float position[3];
		float radius;
		float direction[3];
		float cos_spot_angle; // -2.0 for omni lights.
		float size;
		float near;
		uint32_t slot;
		uint32_t pad;
	};
	static_assert(sizeof(Light) == 48);

	static constexpr uint32_t ENTRY_SIZE = sizeof(uint32_t) * 4; // Per pixel, in the visibility buffer.

private:
	struct Params {
		float inv_projection[16];
		float view_to_world[16];
		int32_t screen_size[2];
		uint32_t light_count;
		uint32_t frame;
		uint32_t soft_samples;
		uint32_t instance_mask;
		float normal_bias;
		float pad;
	};

	RtShadowsShaderRD shader;
	RID shader_version;
	RID pipeline;
	RID params_buffer;
	RID lights_buffer;
	uint32_t lights_capacity = 0;

public:
	RTShadows();
	~RTShadows();

	// p_layers: the renderer's layer table (light caster masks, then instance layers; see rt_caster_mask_inc.glsl).
	void trace(RID p_tlas, RID p_layers, RID p_depth, RID p_normal_roughness, RID p_visibility_buffer, const Size2i &p_size, const Projection &p_projection, const Transform3D &p_cam_transform, const LocalVector<Light> &p_lights, uint32_t p_instance_mask, uint32_t p_soft_samples, uint32_t p_frame);
};

} // namespace RendererRD
