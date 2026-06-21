# Harmonia

The shared Vulkan foundation for the [Hyperion](https://github.com/McNopper/Hyperion)
(offline) and [Theia](https://github.com/McNopper/Theia) (real-time) renderers —
everything common **except** the renderer itself and the [Aether](https://github.com/McNopper/Aether)
file format.

> *[Harmonia](https://en.wikipedia.org/wiki/Harmonia) — Greek goddess of harmony and
> concord, who reconciles opposing forces into a unified whole.* Harmonia **unifies** the
> two renderers on one foundation: Vulkan core, GPU scene upload, color management,
> tonemapping, SDR/HDR presentation.

> ⚠️ **Early stage / work in progress.**

---

## Scope

- **Application host** — `harmonia::App`: window, swapchain, HDR render target,
  tonemap + present loop, IBL probe wiring, scene loading and offscreen (headless)
  rendering; renderers plug in via the `harmonia::IRenderer` interface
- **Vulkan core** — device/context (incl. optional ray-tracing and `VK_EXT_mesh_shader`
  when supported), buffers, images, command pools, queues, barriers, logging, timers
- **Shared GPU types** — `GpuVertex`, `GpuMaterial`, `GpuLight`, `GpuEmissiveTriangle`,
  `CameraData`, `PushConstants` and the `Tonemapper` / `LightType` enums
- **GPU upload primitives** — buffers, bindless textures, images (with mip support),
  procedural geometry, IBL probe precompute
- **Color management** — selectable linear Rec.2020 (default) or Rec.709 working color
  space, color-space conversions on asset load
- **Tonemapping** — AgX, ACES, Reinhard, Hable
- **Presentation** — swapchain, surface, SDR / HDR10 / scRGB output color spaces
- **Shared render utilities** — camera, descriptors, pipeline, acceleration structure
- **Shader toolchain** — the `compile_slang_shaders` CMake rule (Slang → SPIR-V at build
  time, used identically by Harmonia, Hyperion and Theia) and the shared SPIR-V loader
  (`harmonia::createShaderModule`)
- **Shared Slang modules** — `bsdf_shared.slang` (OpenPBR BSDF utilities: Fujii diffuse,
  GGX, Fresnel, sheen, lobe weight helpers), `env_sample.slang` (pure-parameter CDF
  env-map importance sampling), `path_integrator.slang` (renderer-agnostic unidirectional
  path-integrator surface estimator — emissive/env NEE + MIS + Russian roulette, shared
  1:1 between Hyperion's path tracer and Theia's RT-GI compute stage via an `ITracer`
  abstraction), `math.slang` (sampling, RNG, GGX helpers), `env.slang`
  (env-map wrappers); renderers import these via `-I ${HARMONIA_SHADER_SOURCE_DIR}` and
  add renderer-specific code on top — no shader duplication between Hyperion and Theia
- **Image I/O** — PNG/JPEG load ([OpenImageIO](https://openimageio.readthedocs.io/)),
  EXR load/save with chromaticities (OpenImageIO, OpenEXR transitively); textures and
  IBL probes are colour-space-converted on load

It does **not** contain a renderer or a renderer's GPU scene. Hyperion provides the
offline path tracer, Theia the real-time forward renderer; both demos are thin
subclasses of `harmonia::App` that inject their renderer through `harmonia::IRenderer`,
and **each renderer owns its own `Scene` and `GpuInstance` layout** — Hyperion's is built
around index buffers and a ray-tracing pipeline, Theia's around meshlets and mesh shaders.
Only types and code that are shared **1:1** live here; anything that diverges per renderer
stays in the renderer.

---

## Building

**Requirements:** Vulkan SDK 1.4, CMake 3.28+, Ninja, clang-cl, vcpkg.

```bash
cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang-cl \
      -DCMAKE_CXX_COMPILER=clang-cl \
      -DCMAKE_TOOLCHAIN_FILE="<vcpkg-root>/scripts/buildsystems/vcpkg.cmake"

cmake --build build
```

## Tests

```bash
cd build && ctest --output-on-failure
```

## Shared denoiser stage

The scene pipeline includes a shared denoiser stage before tone mapping (when
`stages.denoiser` is enabled). It filters accumulated HDR with guide buffers
(`gNormal`/`gDepth`) and can optionally blend history in fixed-view mode.

Runtime tuning flags:

- `--denoiser-strength <0..1>` spatial filter strength (default `0.45`)
- `--denoiser-iterations <1..8>` spatial passes (default `2`)
- `--denoiser-history-blend <0..1>` temporal blend amount (default `0.15`)
- `--denoiser-no-history` / `--denoiser-history` disable/enable temporal blend

## Consuming Harmonia

Hyperion and Theia pull Harmonia (and, transitively, Aether) via CMake `FetchContent`
and link the `harmonia::harmonia` target.

---

## Dependencies

| Library | Purpose |
|---------|---------|
| [Aether](https://github.com/McNopper/Aether) | Scene & material file formats (`.scene.toml` / `.materials.toml` / OBJ) — GPU-agnostic CPU data |
| [Vulkan SDK](https://vulkan.lunarg.com/) | Vulkan 1.4 API, Slang compiler (`slangc`), SDL3, volk |
| [VMA](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | GPU memory allocation |
| [GLM](https://github.com/g-truc/glm) | Mathematics |
| [OpenImageIO](https://openimageio.readthedocs.io/) | Image I/O — PNG/JPEG/EXR load and save; EXR chromaticities for working color-space metadata |
| [Slang](https://shader-slang.com/) | Shader language (Slang → SPIR-V); shared modules compiled via `compile_slang_shaders` |
| [Google Test](https://github.com/google/googletest) | Testing |
