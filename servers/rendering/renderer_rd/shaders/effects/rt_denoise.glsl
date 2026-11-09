#[compute]

#version 450

#VERSION_DEFINES

// Denoiser for a single channel ray traced signal in [0, 1] (one value per pixel, noisy from a few rays).
// MODE_TEMPORAL: reprojects last frame's accumulated signal with the camera motion, rejects it where depth or normal
// do not match, clamps it to the statistics of the current frame's neighborhood (so a change in the scene, such as
// a character walking past, does not leave a trail) and blends the new frame in with a weight of 1 / history length.
// It also writes this frame's guide (world normal and linear depth) for the next frame.
// MODE_SPATIAL: one edge-aware a-trous iteration (5x5 B3 spline taps at a step), weighted by depth plane distance and
// normal; it filters fully where the history is short and fades out where the history is long.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform Params {
	mat4 inv_projection; // Current frame, clip (reverse Z, Y flipped) to view.
	mat4 view_to_world; // Current frame.
	mat4 reprojection; // Current view to last frame's view.
	mat4 prev_projection; // Last frame, view to clip (reverse Z, Y flipped).
	ivec2 screen_size;
	uint history_valid;
	float max_history;
	float max_history_dynamic;
	float clamp_sigma;
	float pad0;
	float pad1;
}
params;

layout(push_constant, std430) uniform PushConstant {
	int step;
	float fade_start; // Spatial: history length from which the filter starts fading out...
	float fade_end; // ...and at which it is off.
	float pad;
}
push;

layout(set = 1, binding = 0) uniform sampler2D source_depth;
layout(set = 1, binding = 1) uniform sampler2D source_normal_roughness;

vec3 view_position(ivec2 p_pixel, float p_depth) {
	vec2 uv = (vec2(p_pixel) + 0.5) / vec2(params.screen_size);
	vec4 pos = params.inv_projection * vec4(uv * 2.0 - 1.0, p_depth, 1.0);
	return pos.xyz / pos.w;
}

#ifdef MODE_TEMPORAL

layout(set = 2, binding = 0) uniform sampler2D source_signal; // Noisy, this frame.
layout(set = 2, binding = 1) uniform sampler2D history_signal; // Last frame: signal, history length.
layout(set = 2, binding = 2) uniform sampler2D history_guide; // Last frame: world normal, linear depth.
layout(rg16f, set = 2, binding = 3) uniform restrict writeonly image2D dest_signal;
layout(rgba16f, set = 2, binding = 4) uniform restrict writeonly image2D dest_guide;

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.screen_size))) {
		return;
	}

	float depth = texelFetch(source_depth, pixel, 0).r;
	float current = texelFetch(source_signal, pixel, 0).r;
	if (depth == 0.0) { // Sky.
		imageStore(dest_signal, pixel, vec4(current, 0.0, 0.0, 0.0));
		imageStore(dest_guide, pixel, vec4(0.0, 0.0, 0.0, 0.0));
		return;
	}

	vec4 normal_roughness = texelFetch(source_normal_roughness, pixel, 0);
	bool dynamic = normal_roughness.w > 0.5; // Encoded by the depth pre-pass: the instance moved or deforms.
	vec3 view_pos = view_position(pixel, depth);
	float linear_depth = -view_pos.z;
	vec3 normal = normalize(mat3(params.view_to_world) * normalize(normal_roughness.xyz * 2.0 - 1.0));

	// Neighborhood statistics of this frame's samples, on the same surface only.
	float sum_w = 0.0;
	float sum_w2 = 0.0;
	float sum = 0.0;
	float sum_sq = 0.0;
	for (int y = -2; y <= 2; y++) {
		for (int x = -2; x <= 2; x++) {
			ivec2 q = clamp(pixel + ivec2(x, y), ivec2(0), params.screen_size - 1);
			float q_depth = texelFetch(source_depth, q, 0).r;
			float q_linear = -view_position(q, q_depth).z;
			float w = (q_depth != 0.0 && abs(q_linear - linear_depth) < 0.05 * linear_depth) ? 1.0 : 0.0;
			float v = texelFetch(source_signal, q, 0).r;
			sum += v * w;
			sum_sq += v * v * w;
			sum_w += w;
			sum_w2 += w * w;
		}
	}
	float mean = sum / max(sum_w, 1.0);
	float variance = max(0.0, sum_sq / max(sum_w, 1.0) - mean * mean);
	float effective_n = sum_w * sum_w / max(sum_w2, 1.0);
	float sigma_mean = sqrt(variance / max(effective_n, 1.0));

	// Reproject: where was this point last frame?
	vec3 prev_view = (params.reprojection * vec4(view_pos, 1.0)).xyz;
	vec4 prev_clip = params.prev_projection * vec4(prev_view, 1.0);
	vec2 prev_uv = (prev_clip.xy / prev_clip.w) * 0.5 + 0.5;
	float prev_linear = -prev_view.z;

	float history = 0.0;
	float history_length = 0.0;
	if (params.history_valid != 0u && all(greaterThanEqual(prev_uv, vec2(0.0))) && all(lessThan(prev_uv, vec2(1.0)))) {
		// Bilinear taps, each kept only if it saw the same surface.
		vec2 texel = prev_uv * vec2(params.screen_size) - 0.5;
		ivec2 base = ivec2(floor(texel));
		vec2 f = texel - vec2(base);
		float weights[4] = float[](
				(1.0 - f.x) * (1.0 - f.y),
				f.x * (1.0 - f.y),
				(1.0 - f.x) * f.y,
				f.x * f.y);
		ivec2 offsets[4] = ivec2[](ivec2(0, 0), ivec2(1, 0), ivec2(0, 1), ivec2(1, 1));
		float total = 0.0;
		float tolerance = 0.03 * prev_linear + 0.01;
		for (int i = 0; i < 4; i++) {
			ivec2 q = clamp(base + offsets[i], ivec2(0), params.screen_size - 1);
			vec4 guide = texelFetch(history_guide, q, 0);
			bool same = guide.w > 0.0 && abs(guide.w - prev_linear) < tolerance && dot(guide.xyz, normal) > 0.8;
			float w = same ? weights[i] : 0.0;
			vec2 h = texelFetch(history_signal, q, 0).rg;
			history += h.r * w;
			history_length += h.g * w;
			total += w;
		}
		if (total > 0.01) {
			history /= total;
			history_length /= total;
		} else {
			history_length = 0.0;
		}
	}

	float result = current;
	float length_out = 1.0;
	if (history_length > 0.0) {
		float box = params.clamp_sigma * sigma_mean + 0.02;
		history = clamp(history, mean - box, mean + box);
		length_out = min(history_length + 1.0, dynamic ? params.max_history_dynamic : params.max_history);
		result = mix(history, current, 1.0 / length_out);
	}

	imageStore(dest_signal, pixel, vec4(result, length_out, 0.0, 0.0));
	imageStore(dest_guide, pixel, vec4(normal, linear_depth));
}

#endif // MODE_TEMPORAL

#ifdef MODE_SPATIAL

layout(set = 2, binding = 0) uniform sampler2D source_signal; // r: signal.
layout(set = 2, binding = 1) uniform sampler2D source_history; // g: history length.
layout(r16f, set = 2, binding = 2) uniform restrict writeonly image2D dest_signal;

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.screen_size))) {
		return;
	}

	float center = texelFetch(source_signal, pixel, 0).r;
	float depth = texelFetch(source_depth, pixel, 0).r;
	float history_length = texelFetch(source_history, pixel, 0).g;
	float strength = 1.0 - smoothstep(push.fade_start, push.fade_end, history_length);

	float result = center;
	if (depth != 0.0 && strength > 0.0) {
		vec3 view_pos = view_position(pixel, depth);
		vec3 normal = normalize(texelFetch(source_normal_roughness, pixel, 0).xyz * 2.0 - 1.0);
		// Tolerated distance from the pixel's plane, growing with distance like the depth precision does.
		float plane_tolerance = 0.005 + 0.003 * -view_pos.z;
		const float kernel[3] = float[](3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0);
		float sum = 0.0;
		float total = 0.0;
		for (int y = -2; y <= 2; y++) {
			for (int x = -2; x <= 2; x++) {
				ivec2 q = pixel + ivec2(x, y) * push.step;
				if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, params.screen_size))) {
					continue;
				}
				float q_depth = texelFetch(source_depth, q, 0).r;
				if (q_depth == 0.0) {
					continue;
				}
				vec3 q_pos = view_position(q, q_depth);
				vec3 q_normal = normalize(texelFetch(source_normal_roughness, q, 0).xyz * 2.0 - 1.0);
				float plane_distance = abs(dot(normal, q_pos - view_pos));
				float w = kernel[abs(x)] * kernel[abs(y)];
				w *= exp(-plane_distance / plane_tolerance);
				w *= pow(max(0.0, dot(normal, q_normal)), 32.0);
				sum += texelFetch(source_signal, q, 0).r * w;
				total += w;
			}
		}
		if (total > 1e-6) {
			result = mix(center, sum / total, strength);
		}
	}

	imageStore(dest_signal, pixel, vec4(result));
}

#endif // MODE_SPATIAL
