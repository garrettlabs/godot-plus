# godot-plus releases

Each entry names the upstream Godot commit the release is based on and the patches
merged into `main` since the previous release. Version strings follow
`4.8.beta.plus.N.<hash>`; tags are `v4.8-beta-plus.N`. See README.md for the scheme.

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
- **plus-versioning** — `module_config = "plus"` (`aab53f9e10`).

Package: `godot-plus-v4.8-beta-plus.1-win64.zip` — editor, console launcher, runtime DLLs
(`D3D12Core.dll`, `d3d12SDKLayers.dll`, `PhysXGpu_64.dll`, `sl.interposer.dll`,
`sl.common.dll`, `sl.dlss.dll`, `nvngx_dlss.dll`), `template_release` and
`template_debug` for Windows x86-64, and a README.
