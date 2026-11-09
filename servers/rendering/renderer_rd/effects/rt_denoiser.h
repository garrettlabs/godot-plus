/**************************************************************************/
/*  rt_denoiser.h                                                         */
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
#include "servers/rendering/renderer_rd/shaders/effects/rt_denoise.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

// Spatio-temporal denoiser for a single channel ray traced signal in [0, 1] (such as ambient occlusion visibility)
// traced at the internal resolution. Temporal accumulation with camera reprojection, depth and normal rejection
// and a neighborhood clamp, then edge-aware a-trous filtering that is wide where the history is short. Its
// history textures live in the render buffers under the context the caller names; the camera state is in History.
class RTDenoiser {
public:
	struct History {
		Projection prev_projection; // With the depth correction applied.
		Transform3D prev_cam_transform;
		uint64_t last_frame = UINT64_MAX;
		uint32_t index = 0;
		Size2i size;
	};

	struct Settings {
		float max_history = 32.0; // Frames.
		float max_history_dynamic = 6.0; // Frames, on pixels of moving or deforming instances.
		float clamp_sigma = 2.0; // Neighborhood clamp width in standard deviations of the local mean.
		uint32_t spatial_iterations = 3; // Steps 1, 2, 4...
	};

private:
	struct Params {
		float inv_projection[16];
		float view_to_world[16];
		float reprojection[16];
		float prev_projection[16];
		int32_t screen_size[2];
		uint32_t history_valid;
		float max_history;
		float max_history_dynamic;
		float clamp_sigma;
		float pad[2];
	};
	static_assert(sizeof(Params) % 16 == 0);

	struct PushConstant {
		int32_t step;
		float fade_start;
		float fade_end;
		float pad;
	};

	enum Mode {
		MODE_TEMPORAL,
		MODE_SPATIAL,
		MODE_MAX
	};

	RtDenoiseShaderRD shader;
	RID shader_version;
	RID pipelines[MODE_MAX];
	RID params_buffer;

public:
	RTDenoiser();
	~RTDenoiser();

	// Denoises p_signal (R16F, sampled) and returns the filtered R16F texture, owned by the render buffers.
	RID denoise(Ref<RenderSceneBuffersRD> p_render_buffers, const StringName &p_context, History &r_history, RID p_signal, RID p_depth, RID p_normal_roughness, const Projection &p_projection, const Transform3D &p_cam_transform, uint64_t p_frame, const Settings &p_settings);
};

} // namespace RendererRD
