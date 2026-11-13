# godot-plus releases

Each entry names the upstream Godot commit the release is based on and the patches
merged into `main` since the previous release. From v1.0.0 on, godot-plus carries its own
semantic version: tags are `vMAJOR.MINOR.PATCH`, and the binary reports
`<godot version>.plus.<version>.<hash>` (built with `BUILD_NAME=MAJOR.MINOR.PATCH`), so the
upstream base stays visible in the string. The two earlier pre-releases used
`v4.8-beta-plus.N`. See README.md for the scheme.

## Unreleased (next: v1.6.0)

- **rt-shadow-caster-mask** (`c47e4ca00f`, `56ad184934`, `85240a9491`, `fdcc856ea4`) — ray traced shadows honor `Light3D.shadow_caster_mask` (issue #29). Before, every shadow caster in the ray tracing scene occluded every traced omni and spot light, so a light carried inside its holder's mesh (a hand-held lamp, a vehicle light) went completely dark with ray tracing on, while shadow maps simply left the holder out. Each view now keeps a small layer table beside its acceleration structure (the light masks by traced-light slot, then the render layers of every instance, about 4 bytes per instance). A light whose mask leaves out a layer that some shadow caster is on traces with every hit as a candidate and accepts only casters on its layers (at most 32 skipped surfaces per ray; past that the ray counts as unoccluded); every other light keeps the first-hit trace it had. Applies to the traced shadow pass, to volumetric fog lit by traced lights and to transparent surfaces that trace their own shadow rays. On a repro with a spot light and a 0.6 m sphere excluded by the mask, the floor patch beyond the sphere reads 187.4 (lit, as with shadow maps) instead of 0.0 with the sphere in front of the light or around it, on Vulkan and D3D12 alike. Cost on a test level with 60 shadowed lamps and 150 animated characters (1080p, 337 omni and spot lights, the nearest 64 traced): with no light masked, nothing measurable (frame 8.09–8.15 → 8.11–8.14 ms on Vulkan, 8.04–8.06 → 8.09–8.14 on D3D12; the traced shadow, fog and transparent passes within 0.03 ms); with every light leaving out a layer holding the 135 skinned meshes, +0.07 ms in the shadow pass and +0.3 ms in the transparent pass (frame +0.4 / +0.3 ms); with every light leaving out the only layer any caster is on (the worst case: every ray steps over every hit), shadow pass +1.5 ms, fog +0.4, transparent pass +2.1 / +2.0 (frame 8.1 → 11.6 ms Vulkan, 8.1 → 11.1 D3D12). With ray tracing off nothing changes.

## v1.5.0 — 2026-11-11

Upstream base: unchanged, `cafc17a192`. A MINOR release: an expanding ring or shell of smoke from one NVIDIA Flow emitter with divergence fade and damping settings, sleeping, impulses and moving-body collision for the CPU cloth, ray traced shadows on transparent surfaces, and three fixes (particles freed under the threaded cull, volumetric fog behind a spot light's caster, SoftBody3D on threaded PhysX). Same package layout as v1.4.0 (runtime DLLs in the root and beside the templates). D3D12 ray tracing still needs the ray-tracing NIR library (`mesa_libs=`), attached again as a release asset.

- **flow-radial-emitter** (`b3fc0e0cbe`) — NVIDIA Flow: three new `PhysXFlow3D` settings, `divergence_fade`, `divergence_damping` and `velocity_damping`, pass through to Flow's advection (their defaults are Flow's own, so existing scenes are unchanged). Flow subtracts the divergence (expansion) channel at only 1.0 per second by default, so a short divergence pulse kept pushing the gas outward for seconds after its emitter stopped; with `divergence_fade` at 50, a pulse of divergence 10 held for three physics ticks stops growing about a quarter of a second after its emitter stops (at the default it was still accelerating after a second). `velocity_damping` sets how fast the gas's motion dies off, e.g. for a dust cloud that should stop and hang.

- **flow-radial-emitter** (`a1e4f19cee`) — NVIDIA Flow: an expanding ring or shell of smoke from one emitter. `PhysXFlowEmitter3D` gains two shapes, `SHAPE_RING` (points on a circle in the node's XZ plane) and `SHAPE_SHELL` (points spread evenly over a sphere), with `point_count` (default 64), `radial_velocity` (m/s outward from the center, default 4) and `point_width` (default two grid cells). They go through Flow's point emitter, so each point pushes the gas along its own direction, which a sphere or box emitter (one velocity for the whole shape) cannot do; before, the same effect took one sphere emitter per point. `velocity`, `temperature`, `fuel`, `burn`, `smoke`, `divergence` and `couple_rate` apply per point; `collision` is not supported for these shapes. The layout is kept until the count, radius, width or the grid's cell changes, and `radius` and `radial_velocity` may be animated. Compared on a 3-tick pulse (ring of 0.5 m, 6 m/s, couple rate 100, 5 cm cells): one 32-point ring and 32 sphere emitters of 0.1 m expand alike (peak radius 0.586 / 0.604 m at 0.2 s, 0.718 / 0.712 m at 1 s), the ring thinner and with about half the smoke; Vulkan and D3D12 give the same images. A pulse needs a high couple rate (at the default of 2 a 3-tick ring is not visible), and outward velocity alone does not grow a closed shell, because the gas is incompressible: points with a `divergence` do (with divergence 10, a shell of 0.5 m reached 2.8 m in 1.5 s at the default `divergence_fade`, and stopped at 0.73 m with a fade of 10).

- **particles-view-axis-threaded-cull** (`e55acb3159`) — freeing a visible `GPUParticles3D` no longer logs `Parameter "particles" is null` from `particles_set_view_axis` (issue #25; the same code is in upstream Godot master). With `rendering/driver/threads/thread_model=2` and more instances than `rendering/limits/spatial_indexer/threaded_cull_minimum_instances`, the scene cull runs on worker threads, which deferred the call through the render command queue, where it could land behind the free of the same particles RID that the main thread had queued after that frame's draw. Each cull thread now records the particles and view axes it found, and the render thread applies them right after the cull's group task completes, so nothing can be dequeued in between. The single-threaded cull still calls it directly. The view axis is applied in the same frame as before and the cull is not serialized.

- **fog-spot-shadow-linear** (`fab08b127a`) — volumetric fog behind the shadow caster of a spot light is no longer lit (issue #28). The spot branch of the volumetric fog shader faded the shadow with the difference of two reverse-Z NDC depths of the light's perspective shadow camera (`z_near = min(0.025, range)`), which shrinks with distance, so the farther a caster stood from the light the less fog it hid (about 2 % of the unshadowed fog shone through at 0.75 m, 32 % at 2.25 m and 82 % at 6 m). Both depths are now converted to linear view depth before the fade, as the omni branch already does with its distance, and the shadow bias is applied in meters. The same fault exists in stock Godot (`volumetric_fog_process.glsl`, spot branch). Ray traced fog shadows, omni, directional and area lights are unchanged.

- **physx-softbody-threaded** (`adc8eaf742`) — `SoftBody3D` on the PhysX backend no longer crashes when `physics/3d/run_on_separate_thread` is on (issue #27). `PhysicsServer3DWrapMT` queues `soft_body_update_rendering_server()` for the physics thread, but the node calls it on the main thread between opening and closing its mesh buffer, so the queued call wrote through a closed (null) buffer and the process died with an access violation within the first second; it happened with the CPU solver and with any rendering thread model. The module now registers a wrapper that runs that one call on the calling thread, and the body hands the render side a copy of its per-vertex positions, normals and bounds, published under a mutex at the end of each step, read-back and rebuild, so the main thread never reads the solver's arrays while the physics thread steps or resizes them. Cost: one copy of the render vertices per body per step (3 to 6 microseconds for a 2,000-vertex body). Nothing changes with physics on the main thread, and no other backend is touched.

- **cloth-sleep-and-impulse** (`674d54b528`, `01960adae1`, `addb71a992`, `9efd441281`) — `PhysXCloth3D` on the CPU solver: sleeping, moving bodies that carry the cloth, point impulses and a thickness setting. A cloth that has come to rest now falls asleep (`can_sleep`, default on; `sleep_speed_threshold`, `sleep_time`; `is_sleeping()`, `wake()`, `sleep()`, signal `sleeping_state_changed`): once, for `sleep_time`, every unpinned vertex stays slower than the threshold, no vertex touches a body that is not static, the anchor is still and the wind is zero, the cloth is no longer stepped, queried vertex by vertex or redrawn. One shape query over its bounds per physics tick wakes it when a body that is not static enters or moves inside them (a StaticBody3D does not); its anchor moving, wind, `reset()`, an impulse or a property that changes how it moves also wake it. A cloth in a non-zero wind never sleeps. The same bounds query comes first for an awake cloth, and the per-vertex collision queries are skipped when no body is in its bounds. On a bench of 144 hanging strips with nothing moving, every strip was asleep 0.8 s after start and the cloth cost of the physics tick fell from 167–171 to 1.7–3.0 ms per second (PhysX and Jolt, two runs each); with 30 capsules and a box walking through the curtains it went from 169–174 to 134–141 ms per second. A vertex touching a moving body now takes that body's velocity at the contact along the contact normal (at least), with friction pulling its sliding velocity toward the body's, so characters and moving platforms carry the cloth instead of passing through it; contacts are kept as planes the solver enforces in every substep of the next step, and a short relaxation follows the collision pass, so a pushed vertex no longer leaves stretched edges. On the bench with 30 walking capsules the largest edge stretch went from 51 % to 3–4 %, on PhysX and Jolt. The collision depth now follows each backend's `rest_info` contract: with Jolt, a vertex whose center was inside a body was not pushed out at all and one just touching was pushed too far (a dropped cloth now rests flat at its radius). New `apply_impulse(position, impulse, radius)`, `apply_force()` and `apply_central_impulse()` queue velocity changes for the next tick, spread over the vertices near the point so the momentum adds up to the impulse, and wake the cloth: the way for a script to let a body too fast for the tick, such as a projectile, move the cloth. New `thickness` sets the collision radius (0, the default, keeps the automatic radius, half the shortest rest edge). The cloth still never pushes bodies. With none of this in use (`can_sleep` off, no bodies near) the simulation is bit-identical to the previous build, and the GPU deformable-surface path is unchanged.

- **rt-transparent-shadows** (`27d48eac06`) — transparent surfaces receive ray traced shadows (issue #24). With ray tracing in effect, omni and spot lights whose shadows are traced have no shadow map, and the ray traced shadow pass traces only from the opaque surfaces of the depth pre-pass; a transparent fragment never matched the depth it traced at, so it was lit as if unshadowed: an alpha-blended panel behind a wall was lit at full strength through it, while an opaque one beside it was in shadow. In the transparent pass, a fragment that is not the surface the pass traced from now traces its own shadow ray to each traced light that reaches it, with an inline ray query in the fragment shader against the frame's scene (same origin offset and near distance as the compute pass, towards the light's side so a surface lit from behind is handled; four rays over the light's disc for lights with a size). Opaque surfaces are unchanged: a specialization constant enables the tracing in the transparent pass only, and the opaque pass's GPU time is unchanged. New setting `rendering/raytracing/shadows/transparent_receivers` (default true) turns it off; unshaded materials, materials that do not receive shadows and lights with a shadow opacity of zero trace nothing. Works on Vulkan and on D3D12 built against the ray-tracing NIR library (ray queries in pixel shaders translate there), and in shader-baked D3D12 exports. With ray tracing off the only change is that the forward shader's fragment stage is compiled as GLSL 460 (test-level images within run-to-run noise of the previous build on both drivers). Cost, as GPU time of the transparent pass at 1080p on an RTX 5070 Ti, setting on against off: a full-screen shaded glass pane under four traced spot lights +0.27 ms (Vulkan and D3D12); a test level with 60 shadowed lamps and 150 animated characters +0.10–0.13 ms in a quiet view, and +1.5 ms in a heavy view with fifteen shaded smoke emitters (overlapping alpha-blended quads, each layer tracing per lamp): frame 6.7–7.2 to 8.3–8.7 ms on Vulkan and 6.7–6.95 to 8.2–8.4 ms on D3D12, 1% low 11–12 to 21–27 ms (four interleaved rounds). With the setting off, GPU times are those of the previous build. Turning the setting off, or disabling shadow reception on particle materials, removes it.

## v1.4.0 — 2026-11-10

Upstream base: unchanged, `cafc17a192`. A MINOR release: ray traced ambient occlusion and ray traced reflections (Vulkan and D3D12, in place of SSAO and SSR when ray tracing is in effect; reflections with material records kept across frames, albedo texture averages at hidden points, reflected-motion reprojection and an optional half-size mode), view-consistent hits for both, a Windows export option that puts the runtime libraries in a subfolder, and a fix for light projectors on lights without a shadow map. Same package layout as v1.0.0, with one addition: the runtime DLLs are also placed beside the export templates in `templates/`, because the Windows exporter copies the Agility SDK, Streamline, PhysX GPU, Blast and Flow libraries from next to the template, and exports from the earlier packages carried none of them. D3D12 ray tracing still needs a build against the ray-tracing NIR library (`mesa_libs=`, see README.md); the release attaches it again as `godot-nir-static-x86_64-msvc-release.zip`, unmodified, with its notice file.

- **rt-reflection-quality** (`f965ba37d2`, `b6747371f6`, `af0ae63ff4`, `81c88e9a74`) — ray traced reflections: material records kept across frames. The per-material records that shade reflected points the camera does not see (albedo, roughness, metallic, emission read from the material's uniforms by name) were read again for every material every frame; they are now kept until the material changes (a parameter, its shader or its next pass), so a run-time change still shows on the next frame. Images are unchanged; render-thread CPU time on a test level with 60 shadowed lamps and 150 animated characters went from 2.00–2.15 ms to 1.92–1.93 ms per frame (three interleaved runs, Vulkan, 1080p). Reflected points shaded from the material record now carry the color of the material's albedo texture as one average color: the `texture_albedo` uniform (BaseMaterial3D's name), else the first `sampler2D` declared with `source_color`, from the material or the shader's default. Its average (linear, a 64x64 grid of bilinear taps on the first mip below 256 texels) is computed by a small compute pass, eight textures per frame, the first time the texture is reflected, and again when the texture's data is replaced; the record's albedo is multiplied by it (a shader with such a texture but no `albedo` uniform reflects the average alone instead of mid gray). Viewport textures are not averaged, and content written into an existing texture keeps its first average. On a synthetic scene with a red-textured and a checker-textured StandardMaterial3D and a custom shader with a `source_color` sampler, all reflected only from the material record, the reflected albedo matches plain materials in the textures' average colors within 2.3 % on Vulkan and D3D12. No load hitch (worst of the first 120 frames unchanged), about 1 MB of VRAM. The reflection denoiser now reprojects the history of smooth surfaces from where the reflected point appeared last frame (the surface point pushed back along the view ray by the reflected ray's length, averaged over the 3x3 neighbors on the same surface), blending back to the surface point between roughness 0.25 and 0.55; the history pixel is accepted where last frame's view ray through it meets this pixel's surface plane at the stored depth and the normal matches. Glossy reflections no longer smear under camera motion: with the camera orbiting a synthetic scene, frames during motion against a converged still frame of the same pose went from 31.5–32.4 dB to 34.7–36.7 dB in the reflection buffer and from 39.8–41.6 dB to 42.1–45.3 dB in the final image (roughness 0.15 and 0.3, Vulkan and D3D12). Mirrors (which keep no history), rough surfaces, the ambient occlusion denoiser and still cameras are unchanged (a still camera reprojects to the same pixel either way). The color denoise pass costs 0.02–0.04 ms more on the test level. The color denoiser's temporal pass no longer gathers its 5x5 neighborhood statistics for pixels that keep no history (mirrors, and pixels whose history was rejected), with identical images (0.09 to 0.07 ms on a synthetic mirror floor; no measurable change on the test level, whose floors are rough). New setting `rendering/raytracing/reflections/half_size` (default false): one ray per 2x2 pixels, traced at the top-left pixel whose depth and normal-roughness are kept as guides, denoised at that size, then brought up to full size by a pass that weights the nine nearest samples by distance and by matching depth, normal and roughness. On the test level at 1080p it brings the reflection passes from 1.65 to 0.54 ms on Vulkan (2.09 to 0.78 ms on D3D12), the frame from 7.2–7.4 to 6.2–6.3 ms (D3D12 6.9 to 5.6 ms) and VRAM down by 85 MB, at the price of grainier glossy reflections and visible steps along the edges of sharp reflections on mirror-like floors. With it off the images are identical to the previous build.

- **fix-light-projector-matrix** (`f3bc5cd8c0`) — light projectors on omni and spot lights that have no shadow map (issue #23). The forward pass finds a projector's texture coordinates with the light's shadow matrix, which was only built for lights that render a shadow map, so a ray traced light, or any light with shadows disabled, projected nothing and lit nothing. The matrix is now built for every omni and spot light with a projector; spot lights use the camera the shadow pass would render with, so the result is the one a shadow-mapped light has. Lights with shadow maps are unchanged. The same fault exists in stock Godot 4.x for a projector light whose shadow was off from its first frame; it went unnoticed because the matrix of a light that had once rendered a shadow stayed in the buffer.

- **rt-view-consistent-hits** (`e28c010989`) — ray traced ambient occlusion and reflections agree with what the camera sees. Rays see meshes as they were uploaded, so geometry that a material moves or discards for the camera only (a material that lowers walls near the camera, a dissolve, a fade) used to occlude and be reflected at its uploaded shape: a lowered wall still darkened the floor beside it and left a dark line along its cut top, and a smooth floor reflected the full-height wall the view no longer showed. With the new setting `rendering/raytracing/scene/view_consistent_hits` on (the default), a hit that lies on screen where the depth buffer shows something clearly farther (the camera sees past it, so the view did not draw that surface there) is skipped and the ray continues behind it, up to four times; all four depth texels around the hit must agree, with a margin of 0.2 % of the depth plus 5 mm. A surface the view draws is never skipped; hits off screen or hidden behind nearer geometry are kept as before. Ray traced shadows are not changed: they answer for a light, and shadow maps also render the uploaded shape from the light. A mesh that only casts shadows no longer occludes traced ambient occlusion or appears in traced reflections where it is on screen. The reflection shading-tier debug view marks rays that skipped hits. With the setting off the images are those of the previous build. Cost at 1080p on an RTX 5070 Ti on a test level with 60 shadowed lamps and 150 animated characters, three interleaved runs per arm: within run-to-run noise on Vulkan (traced reflections 0.95–1.03 ms on and off, traced AO unchanged) and about +0.02 ms on D3D12 (traced reflections 1.50–1.52 against 1.48–1.51 ms); frame time unchanged within 0.05 ms on both drivers.

- **rt-reflections** (`ca290ed2b5`) — ray traced reflections. With ray tracing in effect and the new setting `rendering/raytracing/reflections/enabled` on (the default), a view whose environment enables SSR gets its specular reflections from rays traced against the scene instead of from marching the depth buffer: what is off screen or hidden behind other geometry is reflected, and there is no fade at the screen edges and no gap where the march loses a surface. One ray per pixel, along the mirror direction on smooth surfaces and spread by the roughness on rougher ones, up to the roughness at which the forward pass stops using reflections. A reflected point that the camera sees takes its color from the previous frame's image, which is exact for any material including custom shaders; any other reflected point is shaded from the material's `albedo`, `roughness`, `metallic`, `emission` and `emission_energy` uniforms (the names BaseMaterial3D uses; textures are not sampled), lit by the view's omni, spot and directional lights with traced shadows for the strongest two (`rendering/raytracing/reflections/shadowed_lights`) and by the ambient light color. The normal at such a point comes from the hit triangle, read through buffer addresses from the mesh buffers the ray tracing scene is built from. The denoiser gains a color variant whose history and blur grow with the surface roughness, so mirrors stay sharp and glossy surfaces smooth without TAA (`rendering/raytracing/reflections/denoise`). The result takes the place of SSR's in the forward pass; SSR does not run while it is replaced, and with the setting off nothing new runs (images identical to the previous build). Cost at 1080p on an RTX 5070 Ti on a test level with 60 shadowed lamps and 150 animated characters: 1.5 ms of GPU time against SSR's 0.14 ms (trace 0.9–1.0, denoise 0.6), +0.65 ms render-thread CPU for the per-instance tables, +118 MB of VRAM; 0.79 ms on a lighter top-down view. Works on Vulkan and on D3D12 (built against the ray-tracing NIR library), and in shader-baked D3D12 exports.

- **rt-ambient-occlusion** (`b38e60a6f7`) — ray traced ambient occlusion. With ray tracing in effect and the new setting `rendering/raytracing/ambient_occlusion/enabled` on (the default), a view whose environment enables SSAO gets its ambient occlusion from rays traced against the scene instead of from the depth buffer: geometry off screen and behind other geometry occludes, there are no halos at depth edges and no thickness guessing. One compute pass after the depth pre-pass traces `rendering/raytracing/ambient_occlusion/rays_per_pixel` (default 2) cosine-weighted rays per pixel, `ssao_radius` long; a new spatio-temporal denoiser (camera reprojection with depth and normal rejection, a neighborhood clamp so moving objects leave no trail, and an edge-aware filter that widens where the history is short) filters the result, which works without TAA or an upscaler (`rendering/raytracing/ambient_occlusion/denoise` turns it off for comparison). The result fills the texture SSAO fills, so `ssao_intensity`, `ssao_power`, `ssao_light_affect`, `ssao_ao_channel_affect` and the SSAO debug view keep working; the traced obscurance is scaled so that an environment tuned for SSAO keeps about the same overall brightness. Shadow-casting geometry occludes; transparent and effect meshes do not. Reflection probes and multiview keep SSAO. With ray tracing off nothing new is compiled or run, and with the setting off nothing new runs (images identical to the previous build). Cost at 1080p on an RTX 5070 Ti, as a budget: about 0.6 ms of GPU time at the default 2 rays on a test level with 60 shadowed lamps and 150 animated characters (1 ray 0.45 ms, 4 rays 0.9 ms; SSAO 0.06 ms), 0.4 ms on a close interior view, and about 55 MB of VRAM. Known limits: for a few frames after a moving object uncovers a surface, that surface is grainy at 1 or 2 rays; the SSAO quality, half-size, blur and fade-out settings are not used by the traced path.

- **windows-library-subfolder** (`323292dc30`, `a2848288d5`) — Windows exports can keep their runtime libraries in a subfolder beside the executable (issue #22). The new Windows export option `application/library_subfolder` (empty by default: libraries stay beside the executable) names a single folder, e.g. `lib`; the export then puts the D3D12 Agility SDK, the Streamline (DLSS) libraries, and every library exported without a target folder of its own (GDExtension libraries and their dependencies, the PhysX module's PhysX GPU, Blast and Flow DLLs) into it, and writes the folder name into the exported project. At run time the engine looks there first and then beside the executable: the D3D12 driver passes the folder to the Agility SDK device factory, Streamline is loaded from it and told to load its plugins from it, the PhysX module loads the PhysX GPU and Flow libraries from it, and a GDExtension library whose own path does not exist is looked for there before the executable's folder. The Blast DLLs are delay-loaded on Windows builds made with MSVC or clang-cl (MinGW keeps load-time linking), so a missing Blast DLL now logs an error and disables Blast destruction instead of stopping the game from starting. A missing optional library behaves as before (the feature switches off) and its message names the subfolder. Files that are not libraries (the PhysX license notice) and the ANGLE, AccessKit and PIX libraries, which the executable imports at load time when present, stay beside the executable.

- **windows-library-subfolder** (`a2848288d5`) — Windows exports now include the Streamline (DLSS) libraries (`sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`, `nvngx_dlss.dll`) when they sit next to the export template, so they no longer need copying by hand; each is optional and nothing is reported when it is absent.

## v1.3.0 — 2026-11-08

Upstream base: unchanged, `cafc17a192`. A MINOR release: ray tracing on the D3D12 driver (it ran on Vulkan only in v1.2.x) and the D3D12 shader baking fix; same package layout as v1.0.0. D3D12 ray tracing needs a build against the ray-tracing NIR library (`mesa_libs=`, see README.md); the release attaches it as `godot-nir-static-x86_64-msvc-release.zip`, unmodified, with a notice file. Builds without it still work and keep shadow maps on D3D12.

- **shader-baker-ray-query** (`26c4836985`) — exporting with shader baking, the D3D12 driver and `rendering/raytracing/enabled` on no longer crashes the editor. The ray traced shadow shader and the volumetric fog shader with ray traced lights use ray queries, which the SPIR-V to DXIL translator behind the D3D12 shader container cannot emit; it crashed (exit 139, no message) about a third of the way through baking. Shader containers now report whether they can compile ray queries (Vulkan yes, D3D12 and Metal no), and the shader baker skips those shaders on the others with one warning per skipped shader. A skipped shader is not needed at run time, because ray tracing reports unavailable on D3D12 in a build without the ray-tracing translator. Vulkan exports bake them as before.

- **d3d12-raytracing** (`8f482620d4`) — ray tracing on D3D12. The D3D12 driver gains acceleration structures (BLAS and TLAS builds, in-place BLAS refits for skinned and blend shape surfaces), inline ray queries in compute shaders and raytracing pipelines, so the ray traced omni/spot shadows and the ray traced volumetric fog shadows now run on D3D12 as on Vulkan. Driver implementation from godotengine/godot#118654 by Skyth (blueskythlikesclouds), ported to this tree; BLAS refit, vertex format queries and the feature checks are added here. It needs a SPIR-V to DXIL translator that can emit ray queries: the `raytracing` branch of [blueskythlikesclouds/godot-nir-static](https://github.com/blueskythlikesclouds/godot-nir-static) (Mesa branch `spirv2dxil-rt`, release `raytracing-rel`); build with `mesa_libs=<folder>/mesa` pointing at it. A build against the stock library still works: ray tracing then reports unavailable on D3D12 and shadow maps stay. Ray queries need DXR tier 1.1 and shader model 6.5; only the shaders that use them are compiled at 6.5. On a test level with 60 shadowed lamps and 150 animated characters at 1080p, D3D12 with ray tracing on ran at 5.3 ms per frame against 5.65 ms with shadow maps (positional shadow draw calls 2,150 → 0; GPU time 4.4 ms against 3.9 ms, render-thread CPU 2.1 ms against 3.1 ms; VRAM +288 MB over D3D12 with shadow maps, against +127 MB on Vulkan). Ray traced volumetric fog and SDFGI give the same images and luminance as on Vulkan, and a build without the ray-tracing library reports ray tracing unavailable and renders as before.

- **d3d12-baker-ray-query** (`ced6fa6c2a`) — with that translator linked, the shader baker compiles the ray traced shadow and volumetric fog shaders for D3D12 exports instead of skipping them, so an exported D3D12 game with ray tracing on starts with them baked (an export with shader baking, D3D12 and ray tracing on finishes without a skip warning and the exported game runs with ray tracing in effect and no shadow draw calls).

- **d3d12-shader-cache-version** (`3e6effe853`) — D3D12 shader containers carry a new format version (2), and the NVIDIA Flow shader cache is keyed by the full build hash instead of the version number. The container layout changed with D3D12 ray tracing; without this, the first D3D12 run of a Flow project upgraded from an earlier build logged "Failed to parse shader container from binary" for every cached Flow shader before recompiling.

## v1.2.1 — 2026-11-07

Upstream base: unchanged, `cafc17a192`. A fix release (ray tracing off cost, volumetric fog with ray traced lights, SDFGI voxelization); same package layout as v1.0.0.

- **rt-off-cost** (`69827f5314`) — ray tracing off costs nothing again. The forward shader's ray traced shadow lookup was compiled in unconditionally, so with `rendering/raytracing/enabled` off (the default) every omni and spot light in every forward pass still read its ray traced slot, branched on it and carried the inlined lookup, which cost GPU time in the opaque and transparent passes. The shader code is now compiled in only when ray tracing is in effect. On a test level with 60 shadowed lamps and 150 animated characters at 1080p (Vulkan) v1.2.0 spent 0.25 to 0.3 ms more GPU time per frame than v1.1.1 (4.7 to 4.85 ms against 4.45 to 4.5, the opaque pass +0.09 ms and the transparent pass +0.19 ms); in a build without the static shadow cache the GPU time was 3.18 ms before the fix and 2.93 ms after, equal to the build without the ray tracing patches (2.92 ms). With ray tracing on nothing changes.

- **rt-fog-shadows** (`cb402b0691`, `1cc2fdefde`) — volumetric fog is shadowed by ray traced lights (fixes #20). With ray tracing on, omni and spot lights chosen for ray traced shadows have no shadow map, but the volumetric fog compute still read its light visibility from the shadow atlas, so fog was lit as if no wall or prop stood in the way (a beam through a slot was not cut by the wall around it). The fog pass now traces one ray from each fog cell to each traced light against the same acceleration structure as the opaque pass; lights without a traced slot and directional lights keep their shadow maps. The ray query code is compiled in only when ray tracing is in effect, so with it off the fog shader is unchanged. Fog shadows from traced lights are hard-edged where a shadow map blurs them and no longer leak a little light behind thin walls.

- **sdfgi-rt-binding** (`62607e9002`) — SDFGI works again (fixes #21, a v1.2.0 regression). The SDFGI voxelization shader variant declared the ray traced shadow buffer binding but its uniform set did not provide it, so every voxelization draw was dropped with "Binding (39), set (1) was not provided" and the cascades stayed empty or stale (visible with any `Environment.sdfgi_enabled`, and when a `PXTGI3D` enables it).

## v1.2.0 — 2026-11-05

Upstream base: unchanged, `cafc17a192`. A MINOR release: ray tracing in the Forward+ renderer (acceleration structures and ray-traced omni/spot shadows, Vulkan only, off by default) plus three fixes; same package layout as v1.0.0.

- **rt-acceleration-structures** (`23adaf0e7c`, `080e5e99d7`) — ray tracing acceleration structures for
  the 3D scene, behind `rendering/raytracing/enabled` (off by default, restart required; in effect with
  Forward+ on a GPU with ray query support, which `RenderingServer.is_raytracing_enabled()` reports).
  One BLAS per mesh surface, built on first use (compressed positions are used as BLAS input directly
  where the GPU accepts them), refittable BLAS for skinned and blend shape surfaces, and one TLAS per
  view and frame holding the instances in view plus those within
  `rendering/raytracing/scene/max_distance` (100 m). New `RenderingDevice.blas_update()` (refit), TLAS
  instances without a hit shader binding table range for ray queries, and Viewport render info for TLAS
  instances, BLAS builds and refits. On a test level with 60 shadowed lamps and 150 animated characters
  at 1080p the scene costs 0.47 ms of GPU time and about 190 MB of VRAM. Particles, 2D meshes and
  non-triangle primitives are not part of it.

- **rt-shadows** (`7608586d52`) — ray-traced shadows for omni and spot lights when ray tracing is in
  effect (`rendering/raytracing/shadows/enabled`, default on): the nearest
  `rendering/raytracing/shadows/max_positional_lights` (64) shadow-casting lights trace their shadows in
  a compute pass after the depth pre-pass instead of rendering shadow maps, so they take no shadow atlas
  space and cost no shadow draws. Lights with a size trace `soft_shadow_samples` (4) rays for soft
  shadows. On the same test level: shadow draw calls 2,150 → 0 and frame 5.74 → 5.32 ms (Vulkan,
  against shadow maps with the static shadow cache), the pass at 0.92 ms of GPU time. Directional
  lights, reflection probes, multiview, D3D12 and GPUs without ray queries keep shadow maps.
  Transparent surfaces receive no ray-traced shadows, light caster and cull masks are not applied, and
  geometry a material displaces in its vertex shader casts its unmodified shape.

- **fix/flow-colliders-in-physics-step** (`8dca5ab80e`, PR #18) — fixes #17: Flow grids now gather their colliders inside the physics step, so they keep their solids when physics runs on its own thread (`3d/run_on_separate_thread`) instead of failing to access the space state.

- **multimesh-interpolation-thread** (`f73f42a48c`) — fixes #16: `RenderingServer` tick and pre-draw are queued to the render thread, so MultiMesh physics interpolation no longer updates buffers from the wrong thread (`buffer_update` errors with `thread_model = 2`).

- **instance-free-material-order** (`f3e52f130b`) — fixes #19 (also godotengine/godot#85817): an instance's materials are cleared before its pending update when it is freed, so freeing a node with surface material overrides no longer logs `Parameter "material" is null` errors.

## v1.1.1 — 2026-11-04

Upstream base: unchanged, `cafc17a192`. A fix release (issue #15 and the DLSS frame token with the separate render thread); same package layout as v1.0.0. Re-cut the same evening with the DLSS fix; the first v1.1.1 package (`81470f4a8f`) carried only the #15 fix.

- **dlss-frame-token** (`d5569dfb13`) — With the separate render thread (`thread_model = 2`) the DLSS pass could set its Streamline constants twice with one frame token for the same viewport, since the token is advanced on the main thread once per iteration while the pass runs on the render thread; Streamline refused the second call (`sl::eErrorDuplicatedConstants`, logged every few hundred frames) and that frame kept the previous constants. Each DLSS viewport now takes a new token when it sees the frame index it already used, and reads the token once per pass.

- **render-thread-shutdown** (`d4a66e6547`) — With the separate render thread (`thread_model = 2`), the main RenderingDevice is now handed back to the main thread when the render thread stops, so it is finalized cleanly at exit instead of logging a render-thread error and leaking two ObjectDB instances.

## v1.1.0 — 2026-11-03

Upstream base: unchanged, `cafc17a192`. A feature release; same package layout as v1.0.0.

- **static-shadow-cache** (`0825aab083`, `a03896ba40`) — static shadow caching for omni and spot lights.
  `GeometryInstance3D.static_shadow_caster` (off by default; `RenderingServer.INSTANCE_FLAG_STATIC_SHADOW_CASTER`)
  marks geometry that never moves; each spot light and dual-paraboloid omni light keeps those casters'
  depth in a second shadow atlas texture and, when something moves in its range, copies that slot
  back and redraws only the other casters. The light, a static caster or the atlas slot changing
  redraws the static set. `rendering/lights_and_shadows/positional_shadow/static_shadow_cache`
  (default on) switches it off. Cube-mode omni lights, area lights and the Mobile renderer keep
  the full redraw. Measured on a test level with 60 shadowed lamps and 150 animated characters at
  1080p: shadow draw calls per frame 5,740 → 745, frame 9.4 → 6.6 ms (Vulkan) and 9.5 → 7.5 ms
  (D3D12), GPU time unchanged. The D3D12 driver no longer asks for a UAV on a copyable depth
  texture, which D3D12 refuses beside the depth-stencil flag.

## v1.0.3 — 2026-11-03

Upstream base: unchanged, `cafc17a192`. Hotfix release; same package layout as v1.0.0.

- **Built with clang-cl** (`use_llvm=yes`, all three targets). The MSVC editor and debug template
  ran GDScript 1.3 to 6 times slower than the official binaries: MSVC compiles the `DEBUG_ENABLED`
  interpreter loop badly (#13). The Clang editor runs the issue's micro benchmark within 20% of
  official 4.7.2; release templates were unaffected and stay the same speed.
- **physx5-module** (`0400c5123d`) — with `physics/3d/run_on_separate_thread`, a direct-state
  query from another thread outside the sync window, or while the space steps, is refused with
  "Space state is inaccessible right now" as Godot Physics and Jolt do, instead of racing PhysX's
  `fetchResults` and crashing in the scene-query tree (#14); `PhysXBlastAuthoring.fracture_mesh_arrays`
  fractures from surface arrays and an image with no RenderingServer call, so a worker thread runs
  at full speed and a main-thread wait on it no longer deadlocks (#12).
- **nav-async-sync** (`542843b5f1`) — a navigation map whose async iterations are switched off while
  a build is in flight collects that build and keeps syncing (#11; also an upstream bug).

## v1.0.2 — 2026-11-02

Upstream base: unchanged, `cafc17a192`. Hotfix release; same package layout as v1.0.0.

- **physx5-module** — two body-motion fixes from the issue tracker (`ebb3ff9776`): a sweep that starts
  touching a surface (within a tenth of the margin) blocks only a motion going into it, so
  `test_move` along the floor a body rests on is clear again (#9, a 1.0.1 regression); a capsule
  or sphere hitting a mesh takes its contact normal from the mesh's closest opposing feature at the
  hit pose instead of PhysX's reported hit point, so a capsule standing against a wall's chamfer
  slides along it instead of freezing for good (#10, present since 1.0.0).

## v1.0.1 — 2026-11-02

Upstream base: unchanged, `cafc17a192`. Hotfix release; same package layout as v1.0.0.

- **physx5-module** — six PhysX fixes from the issue tracker (`83859c577f`, `2161cc8373`): body motion
  honors only the mover's `collision_mask` (#8); a ray starting on a mesh face keeps the face
  normal (#4); `cast_motion` brackets the hit so the unsafe pose touches (#5); the closest-point
  query resolves Area3D RIDs, which `AudioStreamPlayer3D` passes for reverb areas (#2); body motion
  no longer ends inside a concave wall's T-junction (the sweep result is checked at the destination
  and recovery runs to completion) (#6); the last `PXTGI3D` leaving frees the GI effect's pipelines,
  buffers and every viewport's targets (#3).

## v1.0.0 — 2026-11-01

Upstream base: `godotengine/godot` master `cafc17a192` (4.8-beta, 2026-10-27). Binaries report
`4.8.beta.plus.1.0.0.<hash>`.

First full release: everything from the two pre-releases plus the remaining PhysX backends.
The complete set on `main`:

- **streamline-dlss** — DLSS Super Resolution through NVIDIA Streamline 2.10 on Vulkan and
  D3D12 (`4b29b932ef`, `412e813ebc`, `5f84207bc6`).
- **contact-shadows-positional** — screen-space contact shadows for omni and spot lights
  (`a066941c13`).
- **physx5-module** — PhysX 5.10 `PhysicsServer3D` backend with GPU dynamics, vehicles, cloth,
  fluids, water (`f64e8aafc7`, `674bbd0246`, `5782695977`), and new in this release, Blast
  destruction and Flow gas built in (`ed658b07a8`): the Blast code had never compiled against
  4.8 (missing physics-server include, and the `EditorContextMenuPlugin` rework of 78c4f8db69
  changed the fracture menu's signature). Builds pass `blast_sdk=` and `flow_sdk=` from the
  same PhysX checkout; `NvBlast*.dll`, `nvflow.dll` and `nvflowext.dll` ship beside the
  binary. `PhysXDestructible3D`, `PhysXBlastAuthoring`, `PhysXFlow3D` and
  `PhysXFlowEmitter3D` are functional on Vulkan and D3D12.
- **profiling-zones** — Tracy zones (`af05d99b53`); inactive in release builds.
- **plus-versioning** — `module_config = ".plus"` (`aab53f9e10` and the dot fix).

Package: `godot-plus-v1.0.0-win64.zip` — editor, console launcher, runtime DLLs
(`D3D12Core.dll`, `d3d12SDKLayers.dll`, `PhysXGpu_64.dll`, `NvBlast.dll`, `NvBlastGlobals.dll`,
`NvBlastExtAuthoring.dll`, `NvBlastExtShaders.dll`, `nvflow.dll`, `nvflowext.dll`,
`sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`, `nvngx_dlss.dll`), `template_release` and
`template_debug` for Windows x86-64, and a README.

- **physx5-module** — Blast destruction and Flow gas are now built in (`ed658b07a8`): the
  Blast code had never compiled against 4.8 (missing physics-server include, and the
  `EditorContextMenuPlugin` rework of 78c4f8db69 changed the fracture menu's signature). Builds
  pass `blast_sdk=` and `flow_sdk=` from the same PhysX checkout; `NvBlast*.dll`, `nvflow.dll`
  and `nvflowext.dll` ship beside the binary. `PhysXDestructible3D`, `PhysXBlastAuthoring`,
  `PhysXFlow3D` and `PhysXFlowEmitter3D` are functional on Vulkan and D3D12.

## v4.8-beta-plus.2 — 2026-11-01

Upstream base: unchanged, `cafc17a192`.

- **streamline-dlss** — DLSS now runs on the D3D12 driver as well (`5f84207bc6`). The crash
  was the D3D12 driver reusing cached pipeline, root-signature and descriptor-heap state after
  Streamline had rebound its own on the native command list; `RenderingDeviceDriver` gained
  `command_buffer_invalidate_cached_state`, called by the render graph after every driver
  callback. The Vulkan-only fallback from v4.8-beta-plus.1 is removed.

Package: `godot-plus-v4.8-beta-plus.2-win64.zip`, same contents as the first release.

## v4.8-beta-plus.1 — 2026-11-01

Upstream base: `godotengine/godot` master `cafc17a192` (4.8-beta, 2026-10-27).

First release. Patches on `main`:

- **streamline-dlss** — DLSS Super Resolution through NVIDIA Streamline 2.10 on Vulkan
  and D3D12, ported from NVIDIA-RTX/godot (`4b29b932ef`, `412e813ebc`). DLSS is offered
  on Vulkan only in this release: with the D3D12 driver the DLSS viewport mode crashes in
  the GPU driver on the first frame, so D3D12 falls back to FSR 2 with a warning
  (`ccb8ba133b`). Open issue.
- **contact-shadows-positional** — screen-space contact shadows for omni and spot lights,
  capped by `rendering/lights_and_shadows/contact_shadow/max_positional_lights`
  (`a066941c13`).
- **physx5-module** — Wild Ox Studios' PhysX 5.10 `PhysicsServer3D` backend with GPU
  dynamics, vehicles, cloth, fluids, water, Flow and Blast support, imported from
  uno1982/godot and ported to 4.8 (`f64e8aafc7`, `674bbd0246`); automatic CPU worker pool
  sized to a quarter of the cores (`5782695977`). Built with the GPU SDK; Flow and Blast
  SDKs not included in this release.
- **profiling-zones** — Tracy zones for animation, skeletons, scene tree and the render
  path (`af05d99b53`); inactive in release builds.
- **plus-versioning** — `module_config = ".plus"` (`aab53f9e10` and the dot fix).

Package: `godot-plus-v4.8-beta-plus.1-win64.zip` — editor, console launcher, runtime DLLs
(`D3D12Core.dll`, `d3d12SDKLayers.dll`, `PhysXGpu_64.dll`, `sl.interposer.dll`,
`sl.common.dll`, `sl.dlss.dll`, `nvngx_dlss.dll`), `template_release` and
`template_debug` for Windows x86-64, and a README.
