#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "harmonia/GpuTypes.hpp"
#include "harmonia/scene/Geometry.hpp"
#include "harmonia/scene/Material.hpp"

namespace harmonia {

/// Output of the emissive-triangle builder.
struct EmissiveData {
    /// One packed GpuEmissiveTriangle per emissive surface triangle (world-space
    /// vertices baked in; edge1/edge2/normal packed alongside per-triangle radiance).
    /// Always contains at least one sentinel entry so the GPU binding stays valid.
    std::vector<GpuEmissiveTriangle> triangles;
    /// Per-triangle power = area × luminance(Le), used to build a power-proportional
    /// selection CDF.  Size matches triangles before the sentinel is appended.
    std::vector<float> power;
};

/// Build the emissive-triangle buffer and its power vector from the scene.
///
/// Shared implementation used by both Hyperion (path tracer) and Theia (real-time
/// renderer).  Iterates the instance list: for each instance whose mesh is a
/// triangle mesh with an emissive material, the mesh's object-space triangles are
/// transformed by the instance's transform into world space.  The result's
/// `triangles` array is ready for direct GPU upload; pair it with
/// `buildEmissiveCdf(power)` for the NEE selection CDF.
///
/// @param meshes      Unique scene meshes (TriangleMesh / Sphere / …).
/// @param instances   Instance placements (mesh index + transform + material).
/// @param materials   Host material list for emissive-as-light-source checks.
/// @param gpuMaterials GPU material list for emission colour/luminance lookup.
[[nodiscard]] EmissiveData buildEmissiveData(
    const std::vector<std::unique_ptr<Geometry>>& meshes,
    const std::vector<InstanceRecord>&            instances,
    const std::vector<Material>&                  materials,
    const std::vector<GpuMaterial>&               gpuMaterials);

/// Build a power-proportional selection CDF for emissive-triangle NEE:
/// cdf[i] = (Σ_{j≤i} power_j) / totalPower, so cdf[N-1] == 1. Falls back to a uniform
/// CDF when all emitters have zero power (degenerate/black emitters). Always returns
/// at least one entry (a 1.0 sentinel) so the GPU binding stays valid with no emitters.
/// Shared so the NEE sampling CDF cannot drift between renderers.
[[nodiscard]] std::vector<float> buildEmissiveCdf(const std::vector<float>& emissivePower);

} // namespace harmonia
