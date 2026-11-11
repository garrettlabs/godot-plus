#[compute]

#version 450

#VERSION_DEFINES

// Average color of a 2D texture, for ray traced reflections that cannot sample a hit's textures: a 64x64 grid of
// bilinear taps at the first mip whose larger side is below 256 texels (the full image when it has no mips), reduced
// in one work group into one slot of the color table. Colors are linear: an sRGB texture is read through its sRGB view.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source_texture;

layout(set = 0, binding = 1, std430) restrict writeonly buffer Averages {
	vec4 data[];
}
averages;

layout(push_constant, std430) uniform Params {
	uint slot;
	float lod;
	uint pad0;
	uint pad1;
}
params;

shared vec3 partial[64];

void main() {
	uint index = gl_LocalInvocationIndex;
	// Each invocation covers an 8x8 block of the grid.
	vec3 sum = vec3(0.0);
	for (uint y = 0u; y < 8u; y++) {
		for (uint x = 0u; x < 8u; x++) {
			vec2 uv = (vec2(gl_LocalInvocationID.xy * 8u + uvec2(x, y)) + 0.5) / 64.0;
			sum += textureLod(source_texture, uv, params.lod).rgb;
		}
	}
	partial[index] = sum / 64.0;
	barrier();
	for (uint stride = 32u; stride > 0u; stride >>= 1u) {
		if (index < stride) {
			partial[index] += partial[index + stride];
		}
		barrier();
	}
	if (index == 0u) {
		averages.data[params.slot] = vec4(max(partial[0] / 64.0, vec3(0.0)), 1.0);
	}
}
