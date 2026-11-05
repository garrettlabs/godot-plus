# godot-plus releases

Each entry names the upstream Godot commit the release is based on and the patches
merged into `main` since the previous release. From v1.0.0 on, godot-plus carries its own
semantic version: tags are `vMAJOR.MINOR.PATCH`, and the binary reports
`<godot version>.plus.<version>.<hash>` (built with `BUILD_NAME=MAJOR.MINOR.PATCH`), so the
upstream base stays visible in the string. The two earlier pre-releases used
`v4.8-beta-plus.N`. See README.md for the scheme.

## Unreleased (next: v1.1.2)

- **dlss-frame-token** (`d5569dfb13`) — With the separate render thread (`thread_model = 2`) the DLSS pass could set its Streamline constants twice with one frame token for the same viewport, since the token is advanced on the main thread once per iteration while the pass runs on the render thread; Streamline refused the second call (`sl::eErrorDuplicatedConstants`, logged every few hundred frames) and that frame kept the previous constants. Each DLSS viewport now takes a new token when it sees the frame index it already used, and reads the token once per pass.

## v1.1.1 — 2026-11-04

Upstream base: unchanged, `cafc17a192`. A fix release (issue #15); same package layout as v1.0.0.

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
  honours only the mover's `collision_mask` (#8); a ray starting on a mesh face keeps the face
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
