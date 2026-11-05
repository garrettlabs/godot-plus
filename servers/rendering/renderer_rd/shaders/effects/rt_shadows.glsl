#[compute]

#version 460

#VERSION_DEFINES

// glslang only accepts ray queries from GLSL 460 on.
#extension GL_EXT_ray_query : require

// Ray traced shadows for omni and spot lights, one pass after the depth pre-pass. For each pixel, every
// ray traced light whose range covers it gets one shadow ray (several for lights with a size, sampled
// over the light's sphere) and an entry in the pixel's six slots; the forward pass looks lights up by slot.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source_depth;
layout(set = 0, binding = 1) uniform sampler2D source_normal_roughness;
layout(set = 0, binding = 2) uniform accelerationStructureEXT tlas;

struct Light {
	vec3 position;
	float radius;
	vec3 direction; // Spot lights: the cone axis.
	float cos_spot_angle; // -2.0 for omni lights.
	float size; // Light radius for soft shadows, 0.0 for hard shadows.
	float near; // Geometry closer to the light than this does not occlude it (the light's own fixture).
	uint slot;
	uint pad;
};

layout(set = 0, binding = 3, std430) restrict readonly buffer Lights {
	Light data[];
}
lights;

layout(set = 0, binding = 4, std430) restrict writeonly buffer Visibility {
	uvec4 data[];
}
visibility;

layout(set = 0, binding = 5, std140) uniform Params {
	mat4 inv_projection;
	mat4 view_to_world;
	ivec2 screen_size;
	uint light_count;
	uint frame;
	uint soft_samples;
	uint instance_mask;
	float normal_bias;
	float pad;
}
params;

#define MAX_ENTRIES 6u

float hash(vec2 p) {
	// Interleaved gradient noise.
	return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

bool occluded(vec3 origin, vec3 target, float near) {
	vec3 to_target = target - origin;
	float dist = length(to_target);
	float t_max = dist - near;
	if (t_max <= 0.0) {
		return false;
	}
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, params.instance_mask, origin, 0.0, to_target / dist, t_max);
	while (rayQueryProceedEXT(rq)) {
	}
	return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.screen_size))) {
		return;
	}
	uint index = uint(pixel.y) * uint(params.screen_size.x) + uint(pixel.x);

	float depth = texelFetch(source_depth, pixel, 0).r;
	uvec4 result = uvec4(0u, 0u, 0u, floatBitsToUint(depth));
	if (depth == 0.0) {
		// Reverse Z: nothing rendered here (sky).
		visibility.data[index] = result;
		return;
	}

	vec2 uv = (vec2(pixel) + 0.5) / vec2(params.screen_size);
	vec4 view_pos = params.inv_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
	view_pos.xyz /= view_pos.w;
	vec3 world_pos = (params.view_to_world * vec4(view_pos.xyz, 1.0)).xyz;

	vec3 view_normal = normalize(texelFetch(source_normal_roughness, pixel, 0).xyz * 2.0 - 1.0);
	vec3 world_normal = normalize(mat3(params.view_to_world) * view_normal);

	// Offset the origin off the surface, more with distance (depth precision drops with it).
	float view_dist = length(view_pos.xyz);
	vec3 origin = world_pos + world_normal * (params.normal_bias * (1.0 + view_dist * 0.05));

	float noise = hash(vec2(pixel) + float(params.frame % 64u) * 5.588238);

	uint entries = 0u;
	for (uint i = 0u; i < params.light_count && entries < MAX_ENTRIES; i++) {
		Light light = lights.data[i];
		vec3 to_light = light.position - world_pos;
		float dist2 = dot(to_light, to_light);
		if (dist2 >= light.radius * light.radius) {
			continue;
		}
		float dist = sqrt(dist2);
		vec3 light_dir = to_light / max(dist, 1e-6);
		if (light.cos_spot_angle > -1.5 && dot(-light_dir, light.direction) < light.cos_spot_angle) {
			continue; // Outside the spot cone.
		}

		float lit;
		if (light.size > 0.0 && params.soft_samples > 1u) {
			// Points on the disc of the light's sphere that faces the pixel, rotated per pixel.
			vec3 t = normalize(cross(abs(light_dir.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), light_dir));
			vec3 b = cross(light_dir, t);
			float rotation = noise * 6.28318530718;
			lit = 0.0;
			for (uint s = 0u; s < params.soft_samples; s++) {
				// Vogel disc.
				float r = sqrt((float(s) + 0.5) / float(params.soft_samples)) * light.size;
				float a = float(s) * 2.39996323 + rotation;
				vec3 target = light.position + (t * cos(a) + b * sin(a)) * r;
				lit += occluded(origin, target, light.near) ? 0.0 : 1.0;
			}
			lit /= float(params.soft_samples);
		} else {
			lit = occluded(origin, light.position, light.near) ? 0.0 : 1.0;
		}

		uint entry = ((light.slot + 1u) << 8u) | uint(clamp(lit, 0.0, 1.0) * 255.0 + 0.5);
		result[entries >> 1u] |= entry << ((entries & 1u) * 16u);
		entries++;
	}

	visibility.data[index] = result;
}
