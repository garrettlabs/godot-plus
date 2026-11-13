// Shadow caster masks for ray traced shadows (Light3D.shadow_caster_mask).
//
// The includer enables GL_EXT_ray_query, defines RT_CASTER_TLAS as the name of its acceleration structure and declares
// a buffer with the renderer's layer table, named rt_layers:
//
//   buffer RTLayers { uint data[]; } rt_layers;
//
// The table holds RT_LIGHT_MASK_SLOTS light masks, indexed by the light's ray traced shadow slot, followed by the
// render layers of every instance of the acceleration structure, indexed by its instance custom index. A light mask of
// 0xFFFFFFFF means the light's caster mask excludes no shadow caster in the scene: such a light keeps the opaque,
// first-hit query, so lights without a mask cost what they did before masks were honored.

#define RT_LIGHT_MASK_SLOTS 256u
// Hits on excluded instances a masked ray steps over before it gives up and counts as unoccluded. Bounded so that a
// ray through many excluded surfaces cannot stall; a ray inside a holder crosses a few of its triangles.
#define RT_CASTER_MAX_CANDIDATES 32u

uint rt_light_caster_mask(uint p_slot) {
	return rt_layers.data[min(p_slot, RT_LIGHT_MASK_SLOTS - 1u)];
}

// True if a shadow caster on one of p_caster_mask's layers lies on the ray. Without a mask (0xFFFFFFFF) this is the
// opaque, first-hit query the passes used before masks were honored, unchanged. With a mask, every hit is a candidate
// (geometry is forced non-opaque) and only casters on the mask's layers are confirmed.
// Rules learned the hard way, keep them: one exit and no return before a query (an early return turned every result
// into a miss on one Vulkan driver); one rayQueryInitializeEXT per query object, with constant flags (one object
// initialized in two branches read every committed hit as none on D3D12; a single object with run-time flags worked
// but made the transparent pass 0.08 ms slower on D3D12 for unmasked lights); a constant bound on the candidate loop.
bool rt_caster_ray_hit(vec3 p_origin, vec3 p_dir, float p_t_max, uint p_instance_mask, uint p_caster_mask) {
	bool hit = false;
	if (p_caster_mask == 0xFFFFFFFFu) {
		rayQueryEXT rq;
		rayQueryInitializeEXT(rq, RT_CASTER_TLAS, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, p_instance_mask, p_origin, 0.0, p_dir, p_t_max);
		while (rayQueryProceedEXT(rq)) {
		}
		hit = rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
	} else {
		rayQueryEXT rq_masked;
		rayQueryInitializeEXT(rq_masked, RT_CASTER_TLAS, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsNoOpaqueEXT, p_instance_mask, p_origin, 0.0, p_dir, p_t_max);
		for (uint i = 0u; i < RT_CASTER_MAX_CANDIDATES; i++) {
			if (!rayQueryProceedEXT(rq_masked)) {
				break;
			}
			uint index = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq_masked, false));
			if ((rt_layers.data[RT_LIGHT_MASK_SLOTS + index] & p_caster_mask) != 0u) {
				rayQueryConfirmIntersectionEXT(rq_masked);
			}
		}
		hit = rayQueryGetIntersectionTypeEXT(rq_masked, true) != gl_RayQueryCommittedIntersectionNoneEXT;
	}
	return hit;
}
