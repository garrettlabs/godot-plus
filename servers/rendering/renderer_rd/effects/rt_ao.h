/**************************************************************************/
/*  rt_ao.h                                                               */
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
#include "servers/rendering/renderer_rd/shaders/effects/rt_ao.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/rendering_device.h"

#define RB_SCOPE_RT_AO SNAME("rb_rt_ao")
#define RB_SCOPE_RT_AO_DENOISE SNAME("rb_rt_ao_denoise")

namespace RendererRD {

// Ray traced ambient occlusion: a compute pass with ray queries against the view's TLAS after the depth pre-pass,
// denoised, then shaped by the environment's AO intensity and power into the texture screen-space AO fills
// (RB_SCOPE_SSAO / RB_FINAL), so the forward pass and the AO debug view read it unchanged.
class RTAmbientOcclusion {
public:
	// Traced obscurance is larger than what screen-space AO reports for the same scene (SSAO sees only the depth
	// buffer, within its horizon threshold). Scaling it keeps an environment tuned for SSAO at about the same overall
	// brightness: on three test scenes, at their own values and at the defaults, the factor that gives the AO term
	// the same mean as SSAO's was 0.17 to 0.28.
	static constexpr float INTENSITY_SCALE = 0.2f;

	struct Settings {
		float radius = 1.0;
		float intensity = 2.0;
		float power = 1.5;
		uint32_t ray_count = 2;
		uint32_t instance_mask = 0xFF;
		bool denoise = true;
		// Skip hits on screen where the view sees past them (see rt_view_hit_inc.glsl).
		bool view_consistent_hits = true;
		Vector2 taa_jitter; // The jitter the depth buffer was rendered with.
	};

private:
	struct Params {
		float inv_projection[16];
		float view_to_world[16];
		float world_to_clip[16];
		int32_t screen_size[2];
		uint32_t frame;
		uint32_t ray_count;
		float radius;
		float intensity;
		float power;
		uint32_t instance_mask;
		float normal_bias;
		uint32_t view_consistent;
		float pad[2];
	};
	static_assert(sizeof(Params) % 16 == 0);

	enum Mode {
		MODE_TRACE,
		MODE_RESOLVE,
		MODE_MAX
	};

	RtAoShaderRD shader;
	RID shader_version;
	RID pipelines[MODE_MAX];
	RID params_buffer;

	RTDenoiser denoiser;

public:
	RTAmbientOcclusion();
	~RTAmbientOcclusion();

	// Traces, denoises (with the history in r_history) and writes p_destination (R8, storage), all at the internal size.
	void process(Ref<RenderSceneBuffersRD> p_render_buffers, RTDenoiser::History &r_history, RID p_tlas, RID p_depth, RID p_normal_roughness, RID p_destination, const Projection &p_projection, const Transform3D &p_cam_transform, uint64_t p_frame, const Settings &p_settings);
};

} // namespace RendererRD
