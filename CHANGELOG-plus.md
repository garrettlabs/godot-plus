# godot-plus releases

Each entry names the upstream Godot commit the release is based on and the patches
merged into `main` since the previous release. From v1.0.0 on, godot-plus carries its own
semantic version: tags are `vMAJOR.MINOR.PATCH`, and the binary reports
`<godot version>.plus.<version>.<hash>` (built with `BUILD_NAME=MAJOR.MINOR.PATCH`), so the
upstream base stays visible in the string. The two earlier pre-releases used
`v4.8-beta-plus.N`. See README.md for the scheme.

## Unreleased (next: v1.4.0)

- **rt-ambient-occlusion** (`b38e60a6f7`) — ray traced ambient occlusion. With ray tracing in effect and the new setting `rendering/raytracing/ambient_occlusion/enabled` on (the default), a view whose environment enables SSAO gets its ambient occlusion from rays traced against the scene instead of from the depth buffer: geometry off screen and behind other geometry occludes, there are no halos at depth edges and no thickness guessing. One compute pass after the depth pre-pass traces `rendering/raytracing/ambient_occlusion/rays_per_pixel` (default 2) cosine-weighted rays per pixel, `ssao_radius` long; a new spatio-temporal denoiser (camera reprojection with depth and normal rejection, a neighborhood clamp so moving objects leave no trail, and an edge-aware filter that widens where the history is short) filters the result, which works without TAA or an upscaler (`rendering/raytracing/ambient_occlusion/denoise` turns it off for comparison). The result fills the texture SSAO fills, so `ssao_intensity`, `ssao_power`, `ssao_light_affect`, `ssao_ao_channel_affect` and the SSAO debug view keep working; the traced obscurance is scaled so that an environment tuned for SSAO keeps about the same overall brightness. Shadow-casting geometry occludes; transparent and effect meshes do not. Reflection probes and multiview keep SSAO. With ray tracing off nothing new is compiled or run, and with the setting off nothing new runs (images identical to the previous build). Cost at 1080p on an RTX 5070 Ti, as a budget: about 0.6 ms of GPU time at the default 2 rays on a test level with 60 shadowed lamps and 150 animated characters (1 ray 0.45 ms, 4 rays 0.9 ms; SSAO 0.06 ms), 0.4 ms on a close interior view, and about 55 MB of VRAM. Known limits: for a few frames after a moving object uncovers a surface, that surface is grainy at 1 or 2 rays; the SSAO quality, half-size, blur and fade-out settings are not used by the traced path.

- **windows-library-subfolder** — Windows exports can keep their runtime libraries in a subfolder beside the executable (issue #22). The new Windows export option `application/library_subfolder` (empty by default: libraries stay beside the executable) names a single folder, e.g. `lib`; the export then puts the D3D12 Agility SDK, the Streamline (DLSS) libraries, and every library exported without a target folder of its own (GDExtension libraries and their dependencies, the PhysX module's PhysX GPU, Blast and Flow DLLs) into it, and writes the folder name into the exported project. At run time the engine looks there first and then beside the executable: the D3D12 driver passes the folder to the Agility SDK device factory, Streamline is loaded from it and told to load its plugins from it, the PhysX module loads the PhysX GPU and Flow libraries from it, and a GDExtension library whose own path does not exist is looked for there before the executable's folder. The Blast DLLs are delay-loaded on Windows builds made with MSVC or clang-cl (MinGW keeps load-time linking), so a missing Blast DLL now logs an error and disables Blast destruction instead of stopping the game from starting. A missing optional library behaves as before (the feature switches off) and its message names the subfolder. Files that are not libraries (the PhysX license notice) and the ANGLE, AccessKit and PIX libraries, which the executable imports at load time when present, stay beside the executable.

- **windows-library-subfolder** — Windows exports now include the Streamline (DLSS) libraries (`sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`, `nvngx_dlss.dll`) when they sit next to the export template, so they no longer need copying by hand; each is optional and nothing is reported when it is absent.

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
