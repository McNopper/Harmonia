#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "harmonia/GpuTypes.hpp"
#include "harmonia/scene/Geometry.hpp"
#include "harmonia/scene/Material.hpp"

namespace harmonia {

/// Renderer-neutral per-instance descriptor for the emissive builder.
/// Callers fill this from their renderer-specific GpuInstance arrays.
struct EmissiveInstanceInfo {
    uint32_t geometryKind;  ///< 0 = triangle mesh, 1 = analytic sphere
    uint32_t materialIndex; ///< index into the scene's material list
};

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

/// Build the emissive-triangle buffer and its power vector from the scene geometry.
///
/// Shared implementation used by both Hyperion (path tracer) and Theia (real-time
/// renderer).  The result's `triangles` array is ready for direct GPU upload; callers
/// still build the CDF from `power` themselves (the CDF upload is renderer-side).
///
/// @param geometries   Scene geometry list (TriangleMesh / Sphere / …).
/// @param instances    Per-geometry renderer-neutral metadata (kind + material index).
/// @param materials    Host material list for emissive-as-light-source checks.
/// @param gpuMaterials GPU material list for emission colour/luminance lookup.
[[nodiscard]] EmissiveData buildEmissiveData(
    const std::vector<std::unique_ptr<Geometry>>& geometries,
    const std::vector<EmissiveInstanceInfo>&      instances,
    const std::vector<Material>&                  materials,
    const std::vector<GpuMaterial>&               gpuMaterials);

} // namespace harmonia
