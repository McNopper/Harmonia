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

- **Vulkan core** — device/context (incl. optional ray-tracing and `VK_EXT_mesh_shader`
  when supported), buffers, images, command pools, queues, logging, timers
- **Shared GPU types** — `GpuVertex`, `GpuMaterial`, `GpuLight`, `GpuEmissiveTriangle`,
  `CameraData`, `PushConstants` and the `Tonemapper` / `LightType` enums
- **GPU upload primitives** — buffers, bindless textures, images (with mip support),
  procedural geometry, IBL probe precompute
- **Color management** — linear Rec.2020 working space, color-space conversions
- **Tonemapping** — AgX, ACES, Reinhard, Hable
- **Presentation** — swapchain, surface, SDR / HDR10 / scRGB output color spaces
- **Shared render utilities** — camera, descriptors, pipeline, acceleration structure

It does **not** contain a renderer or a renderer's GPU scene. Hyperion provides the
offline path tracer, Theia the real-time forward renderer, and **each renderer owns its
own `Scene` and `GpuInstance` layout** — Hyperion's is built around index buffers and a
ray-tracing pipeline, Theia's around meshlets and mesh shaders. Only types and code that
are shared **1:1** live here; anything that diverges per renderer stays in the renderer.

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

## Consuming Harmonia

Hyperion and Theia pull Harmonia (and, transitively, Aether) via CMake `FetchContent`
and link the `harmonia::harmonia` target.