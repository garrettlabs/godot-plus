# godot-plus

**godot-plus is Godot 4.8 with a set of rendering and physics additions that upstream
does not have**: DLSS through NVIDIA Streamline, screen-space contact shadows for omni and
spot lights, static shadow caching, ray tracing (acceleration structures and ray-traced
omni and spot shadows, on Vulkan and D3D12), a PhysX 5 physics backend with GPU dynamics, Blast
destruction and Flow gas, and profiling zones for Tracy.
`main` tracks upstream master; every addition is kept as its own merged topic branch so the
set rebases onto new upstream releases. The patches are maintained here and are not
submitted upstream.

Upstream's own README follows below under *Godot Engine*.

## What is added

| Feature | Where | Notes |
|---|---|---|
| **DLSS Super Resolution** via NVIDIA Streamline 2.10, on Vulkan and D3D12 | `drivers/streamline/`, `servers/rendering/renderer_rd/effects/dlss.*`, `Viewport.SCALING_3D_MODE_DLSS` | Ported from [NVIDIA-RTX/godot](https://github.com/NVIDIA-RTX/godot) (the DLSS subset only: no path tracer, no Ray Reconstruction). Needs the Streamline runtime DLLs beside the binary and an RTX GPU; otherwise the mode reports unavailable and FSR 2 remains. Project settings under `rendering/streamline/`. (Release v4.8-beta-plus.1 offered DLSS on Vulkan only; `main` since `5f84207bc6` runs it on both drivers.) |
| **Screen-space contact shadows for omni and spot lights** | `renderer_rd/effects/ss_effects.*`, `light_storage.*`, `scene_forward_lights_inc.glsl` | Upstream traces them for directional lights only. `rendering/lights_and_shadows/contact_shadow/max_positional_lights` (default 4) caps the per-frame passes; the nearest lights that allow them get a layer. |
| **PhysX 5.10 physics backend** (`physics/3d/physics_engine = "PhysX"`) with GPU rigid-body dynamics, vehicles, cloth, fluids, water, Flow gas, Blast destruction and a traced GI | `modules/godot_physx/` | Wild Ox Studios' module from [uno1982/godot](https://github.com/uno1982/godot), imported verbatim and ported to 4.8. The PhysX SDK is built out of tree (see Building). Jolt remains the default physics engine; GPU dynamics (`physics/physx_3d/simulation/gpu_dynamics`) pays off with thousands of dynamic bodies and costs a little otherwise. Flow and Blast are built in since `ed658b07a8` (releases from v1.0.0 on). |
| **Static shadow caching** for omni and spot lights: `GeometryInstance3D.static_shadow_caster` marks geometry that never moves and a light then redraws only moving casters over a cached depth of the static ones | `renderer_scene_cull.*`, `renderer_rd/storage_rd/light_storage.*`, `forward_clustered/render_forward_clustered.*` | Forward+, spot and dual-paraboloid omni lights; `rendering/lights_and_shadows/positional_shadow/static_shadow_cache` switches it off. |
| **Ray tracing** in Forward+ (Vulkan and D3D12): acceleration structures for the scene and ray-traced shadows for omni and spot lights, in place of their shadow maps | `renderer_rd/` ray-query passes, `RenderingDevice.blas_update()`, `rendering/raytracing/*` | Off by default (`rendering/raytracing/enabled`, restart required); needs a GPU with ray queries. On D3D12 it needs DXR 1.1, shader model 6.5 and a build against the ray-tracing NIR library (see Building); GPUs or builds without ray-query support keep shadow maps. D3D12 driver support derives from [godotengine/godot#118654](https://github.com/godotengine/godot/pull/118654) by Skyth. Limits: at most 6 traced lights per pixel, light caster and cull masks are not applied, geometry displaced in a vertex shader casts its unmodified shape, and transparent surfaces receive no traced shadows. |
| **Profiling zones** for animation, skeletons, scene tree and the 3D render path | `scene/animation`, `scene/3d`, `scene/main`, `servers/rendering` | Active in `profiler=tracy` builds only. A Tracy capture can then attribute a frame to shadow culling, draw submission, animation and script instead of one opaque zone. |

Everything else is upstream Godot 4.8-beta at the commit `master` points to.

## Upstream bugs fixed here

Bugs that also exist in upstream Godot and are fixed on `main` (numbers are issues in this repository):

- Rendering with `rendering/driver/threads/thread_model=2` logged a render-thread error and leaked two objects at every windowed exit (#15).
- MultiMesh physics interpolation updated buffers off the render thread with `thread_model=2` (#16).
- Freeing an instance with surface material overrides before its first draw logged four `Parameter "material" is null` errors (#19, godotengine/godot#85817).
- A navigation map whose async iterations were switched off while a build was in flight never collected that build and stopped syncing (#11).

## Branches

| Branch | Role |
|---|---|
| `main` | The engine. Upstream `master` plus every patch, merged `--no-ff`; builds and releases come from here. Upstream is pulled in at chosen commits, not continuously, so a build of `main` is reproducible. |
| `master` | Mirror of `godotengine/godot` master. Never patched. |
| `streamline-dlss`, `contact-shadows-positional`, `physx5-module`, `profiling-zones`, `plus-versioning`, `static-shadow-cache`, `rt-acceleration-structures`, `rt-shadows`, `d3d12-raytracing` | One patch each, cut from `master` and merged into `main` once they build and pass testing. |
| `nav-async-sync`, `render-thread-shutdown`, `dlss-frame-token`, `fix/flow-colliders-in-physics-step`, `multimesh-interpolation-thread`, `instance-free-material-order`, `rt-off-cost`, `rt-fog-shadows`, `sdfgi-rt-binding`, `shader-baker-ray-query`, `d3d12-baker-ray-query` | Fix branches, merged into `main` the same way. A fix is cut from the branch it fixes when the code only exists there (`dlss-frame-token` from `streamline-dlss`, `rt-off-cost` from `rt-shadows`, `d3d12-baker-ray-query` from `main`), otherwise from `master`. |

## Building (Windows)

Prerequisites on top of a stock Godot 4.8 Windows editor build (Python 3, SCons ≥ 4.4,
VS 2022 Build Tools with the C++ workload and Windows SDK, the D3D12 Agility SDK bundle
from `misc/scripts/install_d3d12_sdk_windows.py`, AccessKit from
`misc/scripts/install_accesskit.py`):

- **PhysX SDK**: `python modules/godot_physx/misc/build_physx.py --platform windows --gpu --blast --flow`
  clones NVIDIA's PhysX monorepo beside this repo, patches it and installs a static SDK, then
  builds Blast (destruction) and Flow (gas) from the same checkout and prints the scons line.
  `--gpu` needs the CUDA 12.8 toolkit (`winget install Nvidia.CUDA --version 12.8`). If
  CMake then reports *No CUDA toolset found*, copy the four files from
  `%CUDA_PATH%\extras\visual_studio_integration\MSBuildExtensions` into
  `<Build Tools>\MSBuild\Microsoft\VC\v170\BuildCustomizations\`, and make sure
  `CUDA_PATH_V12_8` is set in the shell that runs the build. Flow's own `build.bat` finds
  Visual Studio with `vswhere -latest`, which ignores Build Tools installs; with only Build
  Tools (or a toolset-less Community install) run `generate_projects.bat vs2022` in `flow/`
  and build `_compiler/vs2022/nvflow.sln` (Release) with Build Tools' MSBuild, `CL=/wd4756` set.
- **Streamline DLLs**: `sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`, `nvngx_dlss.dll`
  from the [Streamline SDK 2.10.x release](https://github.com/NVIDIA-RTX/Streamline/releases)
  (`bin/x64/`), copied next to the built binary. The headers are in `thirdparty/streamline`.
- **Ray-tracing NIR library** (optional, for ray tracing on D3D12): download
  `godot-nir-static-x86_64-msvc-release.zip` from this repository's
  [v1.3.0 release assets](https://github.com/garrettlabs/godot-plus/releases/tag/v1.3.0),
  extract it to a folder named `mesa-x86_64-msvc` (so that
  `<dir>/mesa-x86_64-msvc/bin/libNIR.windows.x86_64.lib` exists) and add `mesa_libs=<dir>/mesa`
  to every scons line below. It is a build of
  [godot-nir-static](https://github.com/blueskythlikesclouds/godot-nir-static) with ray-tracing
  support in Mesa's SPIR-V to DXIL translator, by Skyth (blueskythlikesclouds), not yet merged
  upstream, redistributed unmodified under the MIT license. Without it the build succeeds and
  D3D12 simply has no ray tracing (shadow maps stay); Vulkan is unaffected.

Then, from the repo root (PowerShell; Git Bash makes the build look for MinGW), with
`use_llvm=yes` on every target:

```
scons platform=windows target=editor           use_llvm=yes physx_sdk=<sdk>/install/vc17win64-godot-gpu/PhysX physx_gpu=yes blast_sdk=<checkout>/blast/_build/windows-x86_64/release/blast-sdk flow_sdk=<checkout>/flow mesa_libs=<dir>/mesa
scons platform=windows target=template_release use_llvm=yes physx_sdk=<sdk>/install/vc17win64-godot-gpu/PhysX physx_gpu=yes blast_sdk=<checkout>/blast/_build/windows-x86_64/release/blast-sdk flow_sdk=<checkout>/flow mesa_libs=<dir>/mesa
scons platform=windows target=template_debug   use_llvm=yes physx_sdk=<sdk>/install/vc17win64-godot-gpu/PhysX physx_gpu=yes blast_sdk=<checkout>/blast/_build/windows-x86_64/release/blast-sdk flow_sdk=<checkout>/flow mesa_libs=<dir>/mesa
```

`use_llvm=yes` compiles with clang-cl (the "C++ Clang tools for Windows" component of VS 2022,
`VC\Tools\Llvm\x64\bin` on PATH) and names the output `*.llvm.exe`. MSVC builds the
`DEBUG_ENABLED` GDScript interpreter 5 to 6 times slower than Clang does (a typed loop of 2M
iterations: 250 ms against 49; the release template, without `DEBUG_ENABLED`, is unaffected
either way), so an MSVC editor or debug template runs scripts far slower than the official
Godot binaries, which are Clang-built too (issue #13). Releases from v1.0.3 are clang-cl builds.

`physx_sdk=` is required on every build or the PhysX module is silently skipped; without
`blast_sdk=` / `flow_sdk=` the destruction and gas nodes are compiled out. The build copies
`PhysXGpu_64.dll`, `NvBlast.dll`, `NvBlastGlobals.dll`, `NvBlastExtAuthoring.dll`,
`NvBlastExtShaders.dll`, `nvflow.dll`, `nvflowext.dll` and the Agility SDK's `D3D12Core.dll` / `d3d12SDKLayers.dll`
beside the binaries; ship all of them with the executable. A build with
`profiler=tracy profiler_path=<tracy source>` adds the Tracy client (development only).

## Releases and versioning

A release is a build of `main`: editor, console launcher, the runtime DLLs and the two
export templates, all from one commit, packaged as a zip on GitHub Releases.

- **Release version:** godot-plus has its own semantic version, `MAJOR.MINOR.PATCH`,
  independent of the Godot version it is built on. MAJOR changes when the Godot base moves
  to a new minor series or a patch is removed, MINOR when a feature is added or the base
  moves within the series, PATCH for fixes only. The first full release is `v1.0.0`; the two
  `v4.8-beta-plus.N` tags before it were pre-releases.
- **Version string:** Godot's own scheme carries both numbers. `version.py` sets
  `module_config = ".plus"` and the build runs with `BUILD_NAME=MAJOR.MINOR.PATCH`, so the
  editor reports `4.8.beta.plus.1.0.0.<hash>` (upstream version, then the godot-plus version,
  then the commit) and looks for export templates in `export_templates/4.8.beta.plus/`, apart
  from stock 4.8 templates.
- **Release tag:** `vMAJOR.MINOR.PATCH` on `main`. The changelog names the upstream base
  commit of each release.
- **Package name:** `godot-plus-<tag>-win64.zip`, with a README.txt listing its files and
  how to install the templates.
- **Changelog:** `CHANGELOG-plus.md` lists, per release, the upstream base commit and the
  patches merged since the previous release.

## Licensing

Godot is MIT. The PhysX module is MIT (Wild Ox Studios) and links the PhysX SDK
(BSD-3-Clause; notice in `modules/godot_physx/PHYSX-LICENSE.md`). Mesa (the SPIR-V to DXIL
translator, MIT) and godot-nir-static (MIT) are linked into D3D12 builds; the D3D12 ray-tracing
driver code derives from godotengine/godot#118654 by Skyth. Streamline is MIT; the
NVIDIA DLSS runtime (`nvngx_dlss.dll`) is redistributed under the Streamline SDK terms.
Upstream's `LICENSE.txt` and `COPYRIGHT.txt` apply to everything else.

---

# Godot Engine

<p align="center">
  <a href="https://godotengine.org">
    <img src="misc/logo/logo_outlined.svg" width="400" alt="Godot Engine logo">
  </a>
</p>

## 2D and 3D cross-platform game engine

**[Godot Engine](https://godotengine.org) is a feature-packed, cross-platform
game engine to create 2D and 3D games from a unified interface.** It provides a
comprehensive set of [common tools](https://godotengine.org/features), so that
users can focus on making games without having to reinvent the wheel. Games can
be exported with one click to a number of platforms, including the major desktop
platforms (Linux, macOS, Windows), mobile platforms (Android, iOS), as well as
Web-based platforms and [consoles](https://godotengine.org/consoles).

## Free, open source and community-driven

Godot is completely free and open source under the very permissive [MIT license](https://godotengine.org/license).
No strings attached, no royalties, nothing. The users' games are theirs, down
to the last line of engine code. Godot's development is fully independent and
community-driven, empowering users to help shape their engine to match their
expectations. It is supported by the [Godot Foundation](https://godot.foundation/)
not-for-profit.

Before being open sourced in [February 2014](https://github.com/godotengine/godot/commit/0b806ee0fc9097fa7bda7ac0109191c9c5e0a1ac),
Godot had been developed by [Juan Linietsky](https://github.com/reduz) and
[Ariel Manzur](https://github.com/punto-) for several years as an in-house
engine, used to publish several work-for-hire titles.

![Screenshot of a 3D scene in the Godot Engine editor](https://raw.githubusercontent.com/godotengine/godot-design/master/screenshots/editor_tps_demo_1920x1080.jpg)

## Getting the engine

### Binary downloads

Official binaries for the Godot editor and the export templates can be found
[on the Godot website](https://godotengine.org/download).

### Compiling from source

[See the official docs](https://docs.godotengine.org/en/latest/engine_details/development/compiling)
for compilation instructions for every supported platform.

## Community and contributing

Godot is not only an engine but an ever-growing community of users and engine
developers. The main community channels are listed [on the homepage](https://godotengine.org/community).

The best way to get in touch with the core engine developers is to join the
[Godot Contributors Chat](https://chat.godotengine.org).

To get started contributing to the project, see the [contributing guide](CONTRIBUTING.md).
This document also includes guidelines for reporting bugs.

## Documentation and demos

The official documentation is hosted on [Read the Docs](https://docs.godotengine.org).
It is maintained by the Godot community in its own [GitHub repository](https://github.com/godotengine/godot-docs).

The [class reference](https://docs.godotengine.org/en/latest/classes/)
is also accessible from the Godot editor.

We also maintain official demos in their own [GitHub repository](https://github.com/godotengine/godot-demo-projects)
as well as the [Asset Store](https://store.godotengine.org/).

There are also a number of other
[learning resources](https://docs.godotengine.org/en/latest/community/tutorials.html)
provided by the community, such as text and video tutorials, demos, etc.
Consult the [community channels](https://godotengine.org/community)
for more information.

[![Code Triagers Badge](https://www.codetriage.com/godotengine/godot/badges/users.svg)](https://www.codetriage.com/godotengine/godot)
[![Translate on Weblate](https://hosted.weblate.org/widgets/godot-engine/-/godot/svg-badge.svg)](https://hosted.weblate.org/engage/godot-engine/?utm_source=widget)
