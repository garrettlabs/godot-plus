// View-consistent ray hits, shared by ray traced effects that read the view's depth buffer.
//
// Rays see meshes as they were uploaded. A material that moves vertices or discards fragments for the camera only
// draws a surface in one place and traces it in another. A hit point that projects on screen where the depth buffer
// shows something clearly farther lies on a surface the view did not draw there (the camera sees past it), so the
// caller can ignore the hit and continue the ray. A surface the view really drew is never rejected: at its own pixel
// the depth buffer holds that surface or something nearer. Points off screen, in front of the near plane or hidden
// behind nearer geometry cannot be judged and are kept.
//
// The including shader declares `source_depth` (the view's depth buffer, reverse Z, nearest sampling).

// Most hits a ray may skip before it counts as a miss.
#define RT_VIEW_HIT_MAX_SKIPS 4u
// The depth buffer must be farther than the hit by this share of the hit's depth plus this many meters in all four
// texels around the hit. The depth buffer is reverse Z float; the margins cover mesh LOD, packed positions and the
// subpixel jitter of temporal antialiasing, not precision.
#define RT_VIEW_HIT_RELATIVE_MARGIN 0.002
#define RT_VIEW_HIT_ABSOLUTE_MARGIN 0.005

// Linear (positive) view depth of a depth buffer value. A perspective or orthographic projection maps depth through
// z and w alone, so x and y (and the jitter) do not matter.
float rt_view_linear_depth(float p_depth, mat4 p_inv_projection) {
	vec4 pos = p_inv_projection * vec4(0.0, 0.0, p_depth, 1.0);
	return -pos.z / pos.w;
}

// p_world_to_clip: the projection the depth buffer was rendered with (jitter included) times world to view.
// p_inv_projection: inverse of the same projection (clip to view), for linear depth.
bool rt_hit_hidden_from_view(vec3 p_world_pos, mat4 p_world_to_clip, mat4 p_inv_projection, ivec2 p_size) {
	bool hidden = false;
	vec4 clip = p_world_to_clip * vec4(p_world_pos, 1.0);
	if (clip.w > 0.0) {
		vec3 ndc = clip.xyz / clip.w;
		// Reverse Z: 1.0 at the near plane, 0.0 at the far plane.
		if (all(greaterThan(ndc.xy, vec2(-1.0))) && all(lessThan(ndc.xy, vec2(1.0))) && ndc.z >= 0.0 && ndc.z <= 1.0) {
			float hit_depth = rt_view_linear_depth(ndc.z, p_inv_projection);
			float limit = hit_depth * (1.0 + RT_VIEW_HIT_RELATIVE_MARGIN) + RT_VIEW_HIT_ABSOLUTE_MARGIN;
			// The four texels around the point must agree, so a silhouette edge does not flicker.
			vec2 coord = (ndc.xy * 0.5 + 0.5) * vec2(p_size) - 0.5;
			ivec2 base = ivec2(floor(coord));
			ivec2 max_pixel = p_size - 1;
			hidden = true;
			for (int i = 0; i < 4; i++) {
				ivec2 texel = clamp(base + ivec2(i & 1, i >> 1), ivec2(0), max_pixel);
				float scene_depth = rt_view_linear_depth(texelFetch(source_depth, texel, 0).r, p_inv_projection);
				hidden = hidden && scene_depth > limit;
			}
		}
	}
	return hidden;
}

// Where to restart a ray past a skipped hit at distance p_t.
float rt_view_hit_resume(float p_t) {
	return p_t + max(1e-3, p_t * 1e-4);
}
