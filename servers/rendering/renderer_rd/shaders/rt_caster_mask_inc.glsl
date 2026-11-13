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

// True if a shadow caster on one of p_caster_mask's layers lies on the ray. With a mask, every hit is a candidate
// (geometry is forced non-opaque) and only casters on the mask's layers are confirmed. Single exit and no return
// before the query, as for every ray-query helper here: early returns have turned results into misses on one driver.
bool rt_caster_ray_hit(vec3 p_origin, vec3 p_dir, float p_t_max, uint p_instance_mask, uint p_caster_mask) {
	bool masked = p_caster_mask != 0xFFFFFFFFu;
	uint flags = gl_RayFlagsTerminateOnFirstHitEXT | (masked ? gl_RayFlagsNoOpaqueEXT : gl_RayFlagsOpaqueEXT);
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, RT_CASTER_TLAS, flags, p_instance_mask, p_origin, 0.0, p_dir, p_t_max);
	for (uint i = 0u; i < RT_CASTER_MAX_CANDIDATES; i++) {
		if (!rayQueryProceedEXT(rq)) {
			break;
		}
		// Only masked rays see candidates: opaque traversal commits every hit itself.
		uint index = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false));
		if ((rt_layers.data[RT_LIGHT_MASK_SLOTS + index] & p_caster_mask) != 0u) {
			rayQueryConfirmIntersectionEXT(rq);
		}
	}
	return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}
