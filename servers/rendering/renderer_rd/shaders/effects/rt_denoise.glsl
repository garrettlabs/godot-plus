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
// SIGNAL_COLOR: the same for an RGBA signal (such as reflections, premultiplied), with the history length in its own
// texture and both passes scaled by the surface roughness: a mirror keeps one frame and no blur, rough surfaces get
// the full history and kernel. A reflection moves with the reflected point, not with the surface, so on smooth
// surfaces the history is looked up where the reflected (virtual) point was last frame, found from the reflected
// ray's length; the lookup moves back to the surface point as roughness blurs the reflection.

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
	mat4 prev_inv_projection; // SIGNAL_COLOR only: last frame, clip to view.
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

#if defined(MODE_TEMPORAL) && !defined(SIGNAL_COLOR)

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

#endif // MODE_TEMPORAL && !SIGNAL_COLOR

#if defined(MODE_SPATIAL) && !defined(SIGNAL_COLOR)

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

#endif // MODE_SPATIAL && !SIGNAL_COLOR

#ifdef SIGNAL_COLOR

float decode_roughness(float p_w) {
	// The depth pre-pass stores roughness * 127/255, mirrored above 0.5 for dynamic objects.
	float r = p_w > 0.5 ? 1.0 - p_w : p_w;
	return clamp(r / (127.0 / 255.0), 0.0, 1.0);
}

// How much of the history and of the blur a surface of this roughness gets (0 for a mirror, 1 for a rough surface).
float roughness_weight(float p_roughness) {
	return smoothstep(0.04, 0.3, p_roughness);
}

// How far towards the reflected point the history is reprojected (1 for a sharp reflection, 0 once the roughness
// blurs it so much that it follows the surface).
float virtual_weight(float p_roughness) {
	return 1.0 - smoothstep(0.25, 0.55, p_roughness);
}

#endif

#if defined(MODE_TEMPORAL) && defined(SIGNAL_COLOR)

layout(set = 2, binding = 0) uniform sampler2D source_signal; // Noisy, this frame.
layout(set = 2, binding = 1) uniform sampler2D history_signal; // Last frame.
layout(set = 2, binding = 2) uniform sampler2D history_guide; // Last frame: world normal, linear depth.
layout(rgba16f, set = 2, binding = 3) uniform restrict writeonly image2D dest_signal;
layout(rgba16f, set = 2, binding = 4) uniform restrict writeonly image2D dest_guide;
layout(set = 2, binding = 5) uniform sampler2D history_length;
layout(r16f, set = 2, binding = 6) uniform restrict writeonly image2D dest_length;
layout(set = 2, binding = 7) uniform sampler2D source_hit_distance; // Reflected ray length, 0 for no hit.

// Linear depth in last frame's view at which the ray through p_prev_uv meets the plane through p_point with normal
// p_normal (both in last frame's view space); negative if it does not.
float prev_plane_depth(vec2 p_prev_uv, vec3 p_point, vec3 p_normal) {
	vec2 ndc = p_prev_uv * 2.0 - 1.0;
	vec4 a4 = params.prev_inv_projection * vec4(ndc, 1.0, 1.0);
	vec4 b4 = params.prev_inv_projection * vec4(ndc, 0.5, 1.0);
	vec3 a = a4.xyz / a4.w;
	vec3 d = b4.xyz / b4.w - a;
	float denom = dot(p_normal, d);
	float depth = -1.0;
	if (abs(denom) > 1e-6 * length(d)) {
		depth = -(a.z + d.z * (dot(p_normal, p_point - a) / denom));
	}
	return depth;
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.screen_size))) {
		return;
	}

	float depth = texelFetch(source_depth, pixel, 0).r;
	vec4 current = texelFetch(source_signal, pixel, 0);
	if (depth == 0.0) { // Sky.
		imageStore(dest_signal, pixel, current);
		imageStore(dest_length, pixel, vec4(0.0));
		imageStore(dest_guide, pixel, vec4(0.0));
		return;
	}

	vec4 normal_roughness = texelFetch(source_normal_roughness, pixel, 0);
	bool dynamic = normal_roughness.w > 0.5;
	float roughness = decode_roughness(normal_roughness.w);
	float rw = roughness_weight(roughness);
	vec3 view_pos = view_position(pixel, depth);
	float linear_depth = -view_pos.z;
	vec3 view_normal = normalize(normal_roughness.xyz * 2.0 - 1.0);
	vec3 normal = normalize(mat3(params.view_to_world) * view_normal);

	// Neighborhood statistics of this frame's samples, on the same surface only.
	float sum_w = 0.0;
	float sum_w2 = 0.0;
	vec4 sum = vec4(0.0);
	vec4 sum_sq = vec4(0.0);
	for (int y = -2; y <= 2; y++) {
		for (int x = -2; x <= 2; x++) {
			ivec2 q = clamp(pixel + ivec2(x, y), ivec2(0), params.screen_size - 1);
			float q_depth = texelFetch(source_depth, q, 0).r;
			float q_linear = -view_position(q, q_depth).z;
			float w = (q_depth != 0.0 && abs(q_linear - linear_depth) < 0.05 * linear_depth) ? 1.0 : 0.0;
			vec4 v = texelFetch(source_signal, q, 0);
			sum += v * w;
			sum_sq += v * v * w;
			sum_w += w;
			sum_w2 += w * w;
		}
	}
	vec4 mean = sum / max(sum_w, 1.0);
	vec4 variance = max(vec4(0.0), sum_sq / max(sum_w, 1.0) - mean * mean);
	float effective_n = sum_w * sum_w / max(sum_w2, 1.0);
	vec4 sigma_mean = sqrt(variance / max(effective_n, 1.0));

	// Reproject the reflected point: the surface point pushed along the view ray by the reflected ray's length (the
	// mean over the 3x3 neighbors on the same surface that hit something), blended back to the surface point as the
	// roughness rises. Its history pixel shows another point of the same surface, so the depth test expects where last
	// frame's view ray through it meets this pixel's surface plane.
	vec3 reproject_pos = view_pos;
	bool reflected = false;
	float vw = virtual_weight(roughness);
	if (params.history_valid != 0u && rw > 0.0 && vw > 0.0) {
		float hit_sum = 0.0;
		float hit_count = 0.0;
		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
				ivec2 q = clamp(pixel + ivec2(x, y), ivec2(0), params.screen_size - 1);
				float q_depth = texelFetch(source_depth, q, 0).r;
				float q_hit = texelFetch(source_hit_distance, q, 0).r;
				if (q_hit > 0.0 && q_depth != 0.0 && abs(-view_position(q, q_depth).z - linear_depth) < 0.05 * linear_depth) {
					hit_sum += q_hit;
					hit_count += 1.0;
				}
			}
		}
		if (hit_count > 0.0) {
			vec3 view_dir = normalize(view_pos - view_position(pixel, 1.0)); // Also right for orthogonal projections.
			reproject_pos = view_pos + view_dir * (hit_sum / hit_count) * vw;
			reflected = true;
		}
	}

	vec3 prev_view = (params.reprojection * vec4(reproject_pos, 1.0)).xyz;
	vec4 prev_clip = params.prev_projection * vec4(prev_view, 1.0);
	vec2 prev_uv = (prev_clip.xy / prev_clip.w) * 0.5 + 0.5;
	float prev_linear = -prev_view.z;
	if (reflected) {
		vec3 prev_surface = (params.reprojection * vec4(view_pos, 1.0)).xyz;
		prev_linear = prev_plane_depth(prev_uv, prev_surface, mat3(params.reprojection) * view_normal);
	}

	vec4 history = vec4(0.0);
	float length_in = 0.0;
	if (params.history_valid != 0u && rw > 0.0 && (!reflected || (prev_clip.w > 0.0 && prev_linear > 0.0)) && all(greaterThanEqual(prev_uv, vec2(0.0))) && all(lessThan(prev_uv, vec2(1.0)))) {
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
			history += texelFetch(history_signal, q, 0) * w;
			length_in += texelFetch(history_length, q, 0).r * w;
			total += w;
		}
		if (total > 0.01) {
			history /= total;
			length_in /= total;
		} else {
			length_in = 0.0;
		}
	}

	vec4 result = current;
	float length_out = 1.0;
	if (length_in > 0.0) {
		vec4 box = params.clamp_sigma * sigma_mean + 0.02 * max(mean, vec4(0.05));
		history = clamp(history, mean - box, mean + box);
		// A mirror keeps no history (its single ray is exact); rough surfaces keep the most.
		float cap = max(1.0, mix(1.0, dynamic ? params.max_history_dynamic : params.max_history, rw));
		length_out = min(length_in + 1.0, cap);
		result = mix(history, current, 1.0 / length_out);
	}

	imageStore(dest_signal, pixel, result);
	imageStore(dest_length, pixel, vec4(length_out));
	imageStore(dest_guide, pixel, vec4(normal, linear_depth));
}

#endif // MODE_TEMPORAL && SIGNAL_COLOR

#if defined(MODE_SPATIAL) && defined(SIGNAL_COLOR)

layout(set = 2, binding = 0) uniform sampler2D source_signal;
layout(set = 2, binding = 1) uniform sampler2D source_length;
layout(rgba16f, set = 2, binding = 2) uniform restrict writeonly image2D dest_signal;

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.screen_size))) {
		return;
	}

	vec4 center = texelFetch(source_signal, pixel, 0);
	float depth = texelFetch(source_depth, pixel, 0).r;
	vec4 normal_roughness = texelFetch(source_normal_roughness, pixel, 0);
	float rw = roughness_weight(decode_roughness(normal_roughness.w));
	float history_len = texelFetch(source_length, pixel, 0).r;
	float strength = rw * (1.0 - smoothstep(push.fade_start, push.fade_end, history_len));

	vec4 result = center;
	if (depth != 0.0 && strength > 0.0) {
		vec3 view_pos = view_position(pixel, depth);
		vec3 normal = normalize(normal_roughness.xyz * 2.0 - 1.0);
		float plane_tolerance = 0.005 + 0.003 * -view_pos.z;
		const float kernel[3] = float[](3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0);
		vec4 sum = vec4(0.0);
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
				vec4 q_nr = texelFetch(source_normal_roughness, q, 0);
				vec3 q_normal = normalize(q_nr.xyz * 2.0 - 1.0);
				float plane_distance = abs(dot(normal, q_pos - view_pos));
				float w = kernel[abs(x)] * kernel[abs(y)];
				w *= exp(-plane_distance / plane_tolerance);
				w *= pow(max(0.0, dot(normal, q_normal)), 32.0);
				// Neighbors much smoother than this pixel would smear their sharp reflection into it.
				w *= 1.0 - abs(rw - roughness_weight(decode_roughness(q_nr.w)));
				sum += texelFetch(source_signal, q, 0) * w;
				total += w;
			}
		}
		if (total > 1e-6) {
			result = mix(center, sum / total, strength);
		}
	}

	imageStore(dest_signal, pixel, result);
}

#endif // MODE_SPATIAL && SIGNAL_COLOR
