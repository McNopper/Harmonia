#ifndef HARMONIA_SCENE_EMISSIVEBUILDER_HPP
#define HARMONIA_SCENE_EMISSIVEBUILDER_HPP

#include <cstdint>
#include <memory>
#include <slang-math/slang-math.hpp>
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

/// Packed analytic-sphere emitter plus the diagnostics needed to report an instance
/// transform the GPU sphere primitive cannot represent.
struct SphereEmissive {
    /// Tagged-union entry: kind = EmissiveKind::Sphere, v0_area = (world centre, radius).
    GpuEmissiveTriangle entry{};
    /// 4πr² × luminance(Le) — the same unit as the triangle power, so one CDF ranks both.
    float power = 0.0f;
    /// World-space radius written into `entry`; 0 means the sphere was degenerate and
    /// nothing was appended.
    float radius = 0.0f;
    /// The three transformed radius axes |M·(r,0,0)|, |M·(0,r,0)|, |M·(0,0,r)|.
    sm::float3 axisLengths{0.0f, 0.0f, 0.0f};
    /// False when the axes disagree: the instance is a genuine ellipsoid, which neither the
    /// object-space sphere intersection nor the uniform-cone NEE sampler models.  Such an
    /// emitter is **skipped** rather than approximated — `radius` then only reports the
    /// largest measured axis for diagnostics, and the caller must warn.
    bool uniform = true;
    /// True when the entry was actually appended to the emissive light list.  False for a
    /// degenerate (zero-scale) sphere and for an unsupported ellipsoid.
    bool appended = false;
};

/// Pack one analytic-sphere emitter and append it to @p data.
///
/// The traced primitive is intersected in **object space** (see Hyperion's
/// `intersection.slang`), so the instance transform maps the object-space sphere onto a
/// general ellipsoid.  All three transformed radius axes are therefore measured: when they
/// agree the common value is the world radius; when they do not the emitter is an
/// unsupported ellipsoid and the **maximum** axis is used, so the sampled cone still
/// encloses the whole emitter (too wide, never too narrow — no lit direction is missed).
/// The caller is expected to log the `uniform == false` case; this function never silently
/// substitutes a wrong radius without flagging it.
///
/// Degenerate (zero-scale) spheres are skipped: nothing is appended and the returned
/// `radius` is 0.  Otherwise the entry lands in **both** `triangles` and `power`, so the
/// sphere counts towards `emissiveTriangleCount` and takes part in the selection CDF.
///
/// Split out of `buildEmissiveData` so the packing rules are unit-testable without a
/// Vulkan device (a `Sphere` geometry can only be created against one).
SphereEmissive
appendSphereEmissive(EmissiveData& data, float objectRadius, const sm::float4x4& xformMat, sm::float3 emission);

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
[[nodiscard]] EmissiveData buildEmissiveData(const std::vector<std::unique_ptr<Geometry>>& meshes,
                                             const std::vector<InstanceRecord>& instances,
                                             const std::vector<Material>& materials,
                                             const std::vector<GpuMaterial>& gpuMaterials);

/// Build a power-proportional selection CDF for emissive-triangle NEE:
/// cdf[i] = (Σ_{j≤i} power_j) / totalPower, so cdf[N-1] == 1. Falls back to a uniform
/// CDF when all emitters have zero power (degenerate/black emitters). Always returns
/// at least one entry (a 1.0 sentinel) so the GPU binding stays valid with no emitters.
/// Shared so the NEE sampling CDF cannot drift between renderers.
[[nodiscard]] std::vector<float> buildEmissiveCdf(const std::vector<float>& emissivePower);

} // namespace harmonia
#endif // HARMONIA_SCENE_EMISSIVEBUILDER_HPP
