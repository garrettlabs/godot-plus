#[compute]

#version 460

#VERSION_DEFINES

// Ray traced reflections. One ray per pixel along a GGX-sampled direction (the mirror direction on smooth surfaces)
// against the view's TLAS, closest hit. The hit is shaded in two tiers:
// 1. Screen reuse: if the hit point is the surface the depth buffer shows at its screen position, its colour is read
//    from last frame's lit image (exact for any material).
// 2. Table shading: otherwise the hit is lit from a per-instance material record (albedo, roughness, metallic,
//    emission read from the material's uniforms by name), the view's omni, spot and directional lights (the strongest
//    few with a shadow ray) and the ambient light. The hit normal comes from the triangle's positions, read through
//    buffer addresses from the vertex and index buffers the BLAS was built from.
// Output: premultiplied reflection (alpha 1 on a hit, 0 on a miss) and the hit distance.

// glslang only accepts ray queries from GLSL 460 on.
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

#include "../light_data_inc.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source_depth;
layout(set = 0, binding = 1) uniform sampler2D source_normal_roughness;
layout(set = 0, binding = 2) uniform accelerationStructureEXT tlas;

struct Instance {
	uvec2 vertex_address;
	uvec2 index_address; // Zero for non-indexed geometry.
	uint material;
	uint flags;
	uint pad0;
	uint pad1;
};

#define INSTANCE_FLAG_PACKED 1u // Positions R16G16B16A16_UNORM (stride 8), else R32G32B32_SFLOAT (stride 12).
#define INSTANCE_FLAG_INDEX_16 2u
#define INSTANCE_FLAG_GEOMETRY 4u // The addresses are valid.

layout(set = 0, binding = 3, std430) restrict readonly buffer Instances {
	Instance data[];
}
instances;

struct Material {
	vec4 albedo_metallic;
	vec4 emission_roughness;
};

layout(set = 0, binding = 4, std430) restrict readonly buffer Materials {
	Material data[];
}
materials;

layout(set = 0, binding = 5, std430) restrict readonly buffer OmniLights {
	LightData data[];
}
omni_lights;

layout(set = 0, binding = 6, std430) restrict readonly buffer SpotLights {
	LightData data[];
}
spot_lights;

layout(set = 0, binding = 7, std140) uniform DirectionalLights {
	DirectionalLightData data[MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS];
}
directional_lights;

layout(set = 0, binding = 8) uniform sampler2D last_frame;

layout(rgba16f, set = 0, binding = 9) uniform restrict writeonly image2D dest_reflection;
layout(r16f, set = 0, binding = 10) uniform restrict writeonly image2D dest_hit_distance;

layout(set = 0, binding = 11, std140) uniform Params {
	mat4 inv_projection; // Clip (reverse Z, Y flipped) to view.
	mat4 projection; // View to clip.
	mat4 view_to_world;
	mat4 world_to_view;
	mat4 reprojection; // Current view to last frame's clip, for the last frame's image.
	vec4 ambient; // rgb: ambient light (linear, energy applied).
	ivec2 screen_size;
	uint frame;
	uint instance_mask;
	uint omni_count;
	uint spot_count;
	uint directional_count;
	uint shadowed_lights;
	float max_roughness;
	float normal_bias;
	float emission_scale;
	uint debug_view;
	uint screen_reuse;
	uint pad0;
	uint pad1;
	uint pad2;
}
params;

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer Words {
	uint w[];
};

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer Floats {
	float f[];
};

#define M_PI 3.14159265359

float hash12(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

float ign(vec2 p) {
	return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

vec3 view_position(ivec2 p_pixel, float p_depth) {
	vec2 uv = (vec2(p_pixel) + 0.5) / vec2(params.screen_size);
	vec4 pos = params.inv_projection * vec4(uv * 2.0 - 1.0, p_depth, 1.0);
	return pos.xyz / pos.w;
}

float decode_roughness(float p_w) {
	// The depth pre-pass stores roughness * 127/255, mirrored above 0.5 for dynamic objects.
	float r = p_w > 0.5 ? 1.0 - p_w : p_w;
	return clamp(r / (127.0 / 255.0), 0.0, 1.0);
}

// GGX distribution of visible normals (Heitz 2018), in a frame where the normal is +Z.
vec3 sample_vndf(vec3 p_v, float p_alpha, vec2 p_u) {
	vec3 vh = normalize(vec3(p_alpha * p_v.x, p_alpha * p_v.y, p_v.z));
	float lensq = vh.x * vh.x + vh.y * vh.y;
	vec3 t1 = lensq > 0.0 ? vec3(-vh.y, vh.x, 0.0) / sqrt(lensq) : vec3(1.0, 0.0, 0.0);
	vec3 t2 = cross(vh, t1);
	float r = sqrt(p_u.x);
	float phi = 2.0 * M_PI * p_u.y;
	float s1 = r * cos(phi);
	float s2 = r * sin(phi);
	float s = 0.5 * (1.0 + vh.z);
	s2 = (1.0 - s) * sqrt(1.0 - s1 * s1) + s * s2;
	vec3 nh = s1 * t1 + s2 * t2 + sqrt(max(0.0, 1.0 - s1 * s1 - s2 * s2)) * vh;
	return normalize(vec3(p_alpha * nh.x, p_alpha * nh.y, max(0.0, nh.z)));
}

vec3 fetch_position(uvec2 p_address, bool p_packed, uint p_index) {
	vec3 pos;
	if (p_packed) {
		Words words = Words(p_address);
		uint w0 = words.w[p_index * 2u];
		uint w1 = words.w[p_index * 2u + 1u];
		pos = vec3(float(w0 & 0xFFFFu), float(w0 >> 16u), float(w1 & 0xFFFFu)) / 65535.0;
	} else {
		Floats floats = Floats(p_address);
		pos = vec3(floats.f[p_index * 3u], floats.f[p_index * 3u + 1u], floats.f[p_index * 3u + 2u]);
	}
	return pos;
}

uint fetch_index(Instance p_instance, uint p_primitive, uint p_corner) {
	uint i = p_primitive * 3u + p_corner;
	uint index = i;
	if (any(notEqual(p_instance.index_address, uvec2(0u)))) {
		Words words = Words(p_instance.index_address);
		if ((p_instance.flags & INSTANCE_FLAG_INDEX_16) != 0u) {
			uint w = words.w[i >> 1u];
			index = (i & 1u) != 0u ? (w >> 16u) : (w & 0xFFFFu);
		} else {
			index = words.w[i];
		}
	}
	return index;
}

// Single exit: every path runs the query.
bool shadow_visible(vec3 p_origin, vec3 p_dir, float p_length) {
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, params.instance_mask, p_origin, 0.0, p_dir, max(p_length, 1e-4));
	while (rayQueryProceedEXT(rq)) {
	}
	return rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT;
}

float omni_attenuation(float p_distance, float p_inv_range, float p_decay) {
	float nd = p_distance * p_inv_range;
	nd *= nd;
	nd *= nd;
	nd = max(1.0 - nd, 0.0);
	nd *= nd;
	return nd * pow(max(p_distance, 0.0001), -p_decay);
}

#define MAX_SHADOWED 4

// Direct and ambient light at a table-shaded hit (positions and normals in world and view space).
vec3 shade_hit(vec3 p_pos_world, vec3 p_normal_world, vec3 p_pos_view, vec3 p_normal_view, vec3 p_diffuse) {
	vec3 unshadowed = vec3(0.0);
	// The strongest contributions get a shadow ray; they are kept apart until then.
	vec3 top_light[MAX_SHADOWED];
	vec3 top_dir[MAX_SHADOWED]; // World space, towards the light.
	float top_len[MAX_SHADOWED];
	float top_weight[MAX_SHADOWED];
	uint shadowed = min(params.shadowed_lights, uint(MAX_SHADOWED));
	for (uint k = 0u; k < uint(MAX_SHADOWED); k++) {
		top_light[k] = vec3(0.0);
		top_dir[k] = vec3(0.0, 1.0, 0.0);
		top_len[k] = 0.0;
		top_weight[k] = -1.0;
	}

	mat3 view_to_world = mat3(params.view_to_world);
	uint total = params.omni_count + params.spot_count + params.directional_count;
	for (uint i = 0u; i < total; i++) {
		vec3 contrib = vec3(0.0);
		vec3 l_view = vec3(0.0, 1.0, 0.0);
		float l_len = 1e6;
		if (i < params.omni_count + params.spot_count) {
			bool spot = i >= params.omni_count;
			LightData light = spot ? spot_lights.data[i - params.omni_count] : omni_lights.data[i];
			vec3 rel = light.position - p_pos_view;
			float dist = length(rel);
			if (dist * light.inv_radius < 1.0) {
				l_view = rel / max(dist, 1e-5);
				float att = omni_attenuation(dist, light.inv_radius, light.attenuation);
				if (spot) {
					float scos = max(dot(-l_view, light.direction), light.cone_angle);
					float rim = max(1e-4, (1.0 - scos) / (1.0 - light.cone_angle));
					att *= 1.0 - pow(rim, light.cone_attenuation);
				}
				float ndotl = max(dot(p_normal_view, l_view), 0.0);
				contrib = light.color * att * ndotl / M_PI;
				l_len = max(dist - max(light.size, 0.05), 0.0);
			}
		} else {
			DirectionalLightData light = directional_lights.data[i - params.omni_count - params.spot_count];
			l_view = -light.direction;
			float ndotl = max(dot(p_normal_view, l_view), 0.0);
			contrib = light.color * light.energy * ndotl / M_PI;
		}
		float w = dot(contrib, vec3(0.2126, 0.7152, 0.0722));
		if (w <= 0.0) {
			continue;
		}
		// Insert among the strongest; whatever drops out is added unshadowed.
		uint slot = uint(MAX_SHADOWED);
		for (uint k = 0u; k < shadowed; k++) {
			if (w > top_weight[k]) {
				slot = k;
				break;
			}
		}
		if (slot < shadowed) {
			if (top_weight[shadowed - 1u] > 0.0) {
				unshadowed += top_light[shadowed - 1u];
			}
			for (uint k = shadowed - 1u; k > slot; k--) {
				top_light[k] = top_light[k - 1u];
				top_dir[k] = top_dir[k - 1u];
				top_len[k] = top_len[k - 1u];
				top_weight[k] = top_weight[k - 1u];
			}
			top_light[slot] = contrib;
			top_dir[slot] = normalize(view_to_world * l_view);
			top_len[slot] = l_len;
			top_weight[slot] = w;
		} else {
			unshadowed += contrib;
		}
	}

	vec3 light_sum = unshadowed;
	vec3 origin = p_pos_world + p_normal_world * 0.02;
	for (uint k = 0u; k < shadowed; k++) {
		if (top_weight[k] > 0.0 && shadow_visible(origin, top_dir[k], top_len[k])) {
			light_sum += top_light[k];
		}
	}
	return p_diffuse * (light_sum + params.ambient.rgb);
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.screen_size))) {
		return;
	}

	vec4 result = vec4(0.0);
	float hit_distance = 0.0;

	float depth = texelFetch(source_depth, pixel, 0).r;
	vec4 normal_roughness = texelFetch(source_normal_roughness, pixel, 0);
	float roughness = decode_roughness(normal_roughness.w);

	if (depth != 0.0 && roughness < params.max_roughness) {
		vec3 view_pos = view_position(pixel, depth);
		vec3 view_normal = normalize(normal_roughness.xyz * 2.0 - 1.0);
		mat3 to_world = mat3(params.view_to_world);
		vec3 world_pos = (params.view_to_world * vec4(view_pos, 1.0)).xyz;
		vec3 n = normalize(to_world * view_normal);
		vec3 v = normalize(to_world * -view_pos); // Towards the camera.
		if (dot(n, v) < 0.01) {
			n = normalize(n + v * (0.01 - dot(n, v))); // A normal (map) facing away: bend it back to the viewer.
		}

		vec3 t = normalize(cross(abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), n));
		vec3 b = cross(n, t);
		vec3 v_local = vec3(dot(v, t), dot(v, b), dot(v, n));

		vec3 dir;
		float alpha = roughness * roughness;
		if (alpha < 0.002) {
			dir = reflect(-v, n);
		} else {
			float fi = float(params.frame % 1024u);
			vec2 u = fract(vec2(ign(vec2(pixel)), hash12(vec2(pixel) + 7.13)) + fi * vec2(0.7548776662, 0.5698402910));
			vec3 h_local = sample_vndf(v_local, alpha, u);
			vec3 h = t * h_local.x + b * h_local.y + n * h_local.z;
			dir = reflect(-v, h);
			if (dot(dir, n) <= 0.0) {
				dir = reflect(-v, n); // Sampled below the surface: fall back to the mirror direction.
			}
		}
		dir = normalize(dir);

		float view_dist = length(view_pos);
		vec3 origin = world_pos + n * (params.normal_bias * (1.0 + view_dist * 0.05));

		rayQueryEXT rq;
		rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, params.instance_mask, origin, 0.0, dir, 1000.0);
		while (rayQueryProceedEXT(rq)) {
		}
		if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
			hit_distance = rayQueryGetIntersectionTEXT(rq, true);
			vec3 hit_world = origin + dir * hit_distance;
			vec3 hit_view = (params.world_to_view * vec4(hit_world, 1.0)).xyz;

			// Tier 1: the hit is the surface the depth buffer shows there.
			bool reused = false;
			vec3 color = vec3(0.0);
			vec4 clip = params.projection * vec4(hit_view, 1.0);
			if (params.screen_reuse != 0u && clip.w > 0.0) {
				vec2 uv = (clip.xy / clip.w) * 0.5 + 0.5;
				if (all(greaterThanEqual(uv, vec2(0.0))) && all(lessThan(uv, vec2(1.0)))) {
					ivec2 hp = clamp(ivec2(uv * vec2(params.screen_size)), ivec2(0), params.screen_size - 1);
					float scene_depth = -view_position(hp, texelFetch(source_depth, hp, 0).r).z;
					float hit_depth = -hit_view.z;
					if (abs(scene_depth - hit_depth) < 0.02 * hit_depth + 0.05) {
						vec4 prev_clip = params.reprojection * vec4(hit_view, 1.0);
						vec2 prev_uv = (prev_clip.xy / prev_clip.w) * 0.5 + 0.5;
						if (prev_clip.w > 0.0 && all(greaterThanEqual(prev_uv, vec2(0.0))) && all(lessThan(prev_uv, vec2(1.0)))) {
							color = textureLod(last_frame, prev_uv, 0.0).rgb;
							reused = true;
						}
					}
				}
			}

			// Tier 2: table shading.
			uint instance_index = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true);
			Instance inst = instances.data[instance_index];
			Material mat = materials.data[inst.material];
			vec3 hit_normal = -dir;
			if ((inst.flags & INSTANCE_FLAG_GEOMETRY) != 0u) {
				uint prim = rayQueryGetIntersectionPrimitiveIndexEXT(rq, true);
				bool packed = (inst.flags & INSTANCE_FLAG_PACKED) != 0u;
				vec3 p0 = fetch_position(inst.vertex_address, packed, fetch_index(inst, prim, 0u));
				vec3 p1 = fetch_position(inst.vertex_address, packed, fetch_index(inst, prim, 1u));
				vec3 p2 = fetch_position(inst.vertex_address, packed, fetch_index(inst, prim, 2u));
				vec3 on = cross(p1 - p0, p2 - p0);
				mat4x3 o2w = rayQueryGetIntersectionObjectToWorldEXT(rq, true);
				vec3 wn = transpose(inverse(mat3(o2w))) * on;
				float len = length(wn);
				if (len > 1e-20) {
					hit_normal = wn / len;
				}
				if (dot(hit_normal, dir) > 0.0) {
					hit_normal = -hit_normal; // Face the incoming ray.
				}
			}

			if (!reused) {
				vec3 albedo = mat.albedo_metallic.rgb;
				// No specular at the hit: metals reflect as a darker diffuse of their colour.
				vec3 diffuse = albedo * mix(1.0, 0.5, mat.albedo_metallic.a);
				vec3 normal_view = normalize(mat3(params.world_to_view) * hit_normal);
				color = shade_hit(hit_world, hit_normal, hit_view, normal_view, diffuse);
				color += mat.emission_roughness.rgb * params.emission_scale;
			}

			if (params.debug_view == 1u) {
				color = mat.albedo_metallic.rgb;
			} else if (params.debug_view == 2u) {
				color = hit_normal * 0.5 + 0.5;
			} else if (params.debug_view == 3u) {
				color = reused ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
			}
			result = vec4(color, 1.0);
		}
	}

	imageStore(dest_reflection, pixel, result);
	imageStore(dest_hit_distance, pixel, vec4(hit_distance));
}
