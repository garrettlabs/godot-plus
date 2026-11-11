#[compute]

#version 460

#VERSION_DEFINES

// Ray traced ambient occlusion.
// MODE_TRACE: per pixel, a few cosine weighted rays over the hemisphere of the surface normal against the view's TLAS,
// each hit weighted by its distance (a hit at the origin occludes fully, one at the ray length not at all). The
// result is the raw visibility (1.0 = unoccluded), which the denoiser filters.
// MODE_RESOLVE: applies the environment's intensity and power and writes the term the forward pass reads.

#ifdef MODE_TRACE
// glslang only accepts ray queries from GLSL 460 on.
#extension GL_EXT_ray_query : require
#endif

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 5, std140) uniform Params {
	mat4 inv_projection;
	mat4 view_to_world;
	mat4 world_to_clip; // The projection the depth buffer was rendered with, jitter included.
	ivec2 screen_size;
	uint frame;
	uint ray_count;
	float radius;
	float intensity;
	float power;
	uint instance_mask;
	float normal_bias;
	uint view_consistent; // Skip hits the view sees past (see rt_view_hit_inc.glsl).
	float pad1;
	float pad2;
}
params;

#ifdef MODE_TRACE

layout(set = 0, binding = 0) uniform sampler2D source_depth;
layout(set = 0, binding = 1) uniform sampler2D source_normal_roughness;
layout(set = 0, binding = 2) uniform accelerationStructureEXT tlas;
layout(r16f, set = 0, binding = 3) uniform restrict writeonly image2D dest_visibility;

#include "rt_view_hit_inc.glsl"

float ign(vec2 p) {
	// Interleaved gradient noise.
	return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

float hash12(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

vec3 view_position(ivec2 p_pixel, float p_depth) {
	vec2 uv = (vec2(p_pixel) + 0.5) / vec2(params.screen_size);
	vec4 pos = params.inv_projection * vec4(uv * 2.0 - 1.0, p_depth, 1.0);
	return pos.xyz / pos.w;
}

// Distance weighted occlusion of one ray: 1.0 for a hit at the origin, 0.0 for a miss or a hit at the ray length.
// With view-consistent hits, a hit the view sees past is skipped and the ray continues behind it; a ray still
// skipping after the last allowed skip counts as a miss.
// Single exit: every path runs the query (an early return before it once made every ray miss).
float ray_occlusion(vec3 p_origin, vec3 p_dir, float p_length) {
	float len = max(p_length, 1e-4);
	uint traces = params.view_consistent != 0u ? RT_VIEW_HIT_MAX_SKIPS + 1u : 1u;
	float t_min = 0.0;
	float t_hit = -1.0;
	bool searching = true;
	for (uint i = 0u; i < traces; i++) {
		if (searching) {
			rayQueryEXT rq;
			rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, params.instance_mask, p_origin, t_min, p_dir, len);
			while (rayQueryProceedEXT(rq)) {
			}
			searching = false;
			if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
				float t = rayQueryGetIntersectionTEXT(rq, true);
				if (params.view_consistent != 0u && rt_hit_hidden_from_view(p_origin + p_dir * t, params.world_to_clip, params.inv_projection, params.screen_size)) {
					t_min = min(rt_view_hit_resume(t), len);
					searching = true;
				} else {
					t_hit = t;
				}
			}
		}
	}
	float occlusion = 0.0;
	if (t_hit >= 0.0) {
		float t = clamp(t_hit / len, 0.0, 1.0);
		occlusion = 1.0 - t * t;
	}
	return occlusion;
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.screen_size))) {
		return;
	}

	float depth = texelFetch(source_depth, pixel, 0).r;
	float visibility = 1.0;
	if (depth != 0.0) { // Reverse Z: 0.0 is nothing rendered (sky).
		vec3 view_pos = view_position(pixel, depth);

		// Geometric normal from the depth buffer, from the nearer neighbor on each axis (no depth edge).
		ivec2 max_pixel = params.screen_size - 1;
		vec3 pos_l = view_position(clamp(pixel - ivec2(1, 0), ivec2(0), max_pixel), texelFetch(source_depth, clamp(pixel - ivec2(1, 0), ivec2(0), max_pixel), 0).r);
		vec3 pos_r = view_position(clamp(pixel + ivec2(1, 0), ivec2(0), max_pixel), texelFetch(source_depth, clamp(pixel + ivec2(1, 0), ivec2(0), max_pixel), 0).r);
		vec3 pos_d = view_position(clamp(pixel - ivec2(0, 1), ivec2(0), max_pixel), texelFetch(source_depth, clamp(pixel - ivec2(0, 1), ivec2(0), max_pixel), 0).r);
		vec3 pos_u = view_position(clamp(pixel + ivec2(0, 1), ivec2(0), max_pixel), texelFetch(source_depth, clamp(pixel + ivec2(0, 1), ivec2(0), max_pixel), 0).r);
		vec3 dx = abs(pos_r.z - view_pos.z) < abs(view_pos.z - pos_l.z) ? pos_r - view_pos : view_pos - pos_l;
		vec3 dy = abs(pos_u.z - view_pos.z) < abs(view_pos.z - pos_d.z) ? pos_u - view_pos : view_pos - pos_d;

		vec3 view_normal = normalize(texelFetch(source_normal_roughness, pixel, 0).xyz * 2.0 - 1.0);
		vec3 geo_view_normal = cross(dx, dy);
		float geo_len = length(geo_view_normal);
		geo_view_normal = geo_len > 1e-12 ? geo_view_normal / geo_len : view_normal;
		if (dot(geo_view_normal, view_pos) > 0.0) {
			geo_view_normal = -geo_view_normal; // Face the camera.
		}

		mat3 to_world = mat3(params.view_to_world);
		vec3 world_pos = (params.view_to_world * vec4(view_pos, 1.0)).xyz;
		vec3 normal = normalize(to_world * view_normal);
		vec3 geo_normal = normalize(to_world * geo_view_normal);
		if (dot(normal, geo_normal) < 0.0) {
			normal = geo_normal; // Back facing shading normal (double sided material seen from behind).
		}

		// Offset the origin off the surface, more with distance (depth precision drops with it).
		float view_dist = length(view_pos);
		vec3 origin = world_pos + geo_normal * (params.normal_bias * (1.0 + view_dist * 0.05));

		vec3 t = normalize(cross(abs(normal.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), normal));
		vec3 b = cross(normal, t);

		// Per pixel offsets into an R2 sequence that advances every frame, so the temporal filter integrates a
		// low discrepancy set of directions at each pixel.
		vec2 offset = vec2(ign(vec2(pixel)), hash12(vec2(pixel) + 17.31));
		uint count = clamp(params.ray_count, 1u, 8u);
		float occlusion = 0.0;
		for (uint s = 0u; s < count; s++) {
			float n = float((params.frame % 1024u) * count + s);
			vec2 u = fract(offset + n * vec2(0.7548776662, 0.5698402910));
			// Cosine weighted hemisphere.
			float r = sqrt(u.x);
			float phi = u.y * 6.28318530718;
			vec3 dir = t * (r * cos(phi)) + b * (r * sin(phi)) + normal * sqrt(max(0.0, 1.0 - u.x));
			float below = dot(dir, geo_normal);
			if (below < 0.0) {
				dir -= 2.0 * below * geo_normal; // Mirror rays that would enter the surface (normal maps).
			}
			occlusion += ray_occlusion(origin, normalize(dir), params.radius);
		}
		visibility = 1.0 - occlusion / float(count);
	}

	imageStore(dest_visibility, pixel, vec4(visibility));
}

#endif // MODE_TRACE

#ifdef MODE_RESOLVE

layout(set = 0, binding = 0) uniform sampler2D source_visibility;
layout(r8, set = 0, binding = 1) uniform restrict writeonly image2D dest_ao;

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.screen_size))) {
		return;
	}
	float visibility = texelFetch(source_visibility, pixel, 0).r;
	// Same shaping as screen-space AO: obscurance scaled by the intensity and clamped, visibility raised to the power.
	float obscurance = min((1.0 - clamp(visibility, 0.0, 1.0)) * params.intensity, 0.98);
	float ao = pow(clamp(1.0 - obscurance, 0.0, 1.0), params.power);
	imageStore(dest_ao, pixel, vec4(ao));
}

#endif // MODE_RESOLVE
