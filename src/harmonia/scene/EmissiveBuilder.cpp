#include "harmonia/scene/EmissiveBuilder.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <set>
#include <slang-math/slang-math.hpp>
#include <utility>

#include "harmonia/core/Logger.hpp"
#include "harmonia/scene/Geometry.hpp"
#include "harmonia/scene/Mesh.hpp"

namespace harmonia {
namespace {

/// Relative spread the three transformed sphere axes may show and still count as a uniform
/// scale.  Well above the float round-off of the matrix product, well below any anisotropy
/// a scene would author deliberately.
constexpr float kSphereAxisTolerance = 1.0e-3F;

/// Rec.2020 luminance of a scene-referred emission triple.  Shared by the triangle and the
/// sphere power so both land in the same unit and can be ranked by one CDF.
[[nodiscard]] float emissionLuminance(sm::float3 emission) {
    return 0.2627F * emission.r + 0.6780F * emission.g + 0.0593F * emission.b;
}

} // namespace

SphereEmissive
appendSphereEmissive(EmissiveData& data, float objectRadius, const sm::float4x4& xformMat, sm::float3 emission) {
    SphereEmissive packed;

    // The analytic sphere lives at the object-space origin; the instance transform places
    // it.  The intersection shader runs in OBJECT space, so a non-uniform scale produces a
    // genuine ellipsoid — measure all three transformed radius axes instead of assuming the
    // x axis speaks for the other two.
    const sm::float3 worldCenter = static_cast<sm::float3>(xformMat * sm::float4(0.0F, 0.0F, 0.0F, 1.0F));
    const sm::float3 axisX =
        static_cast<sm::float3>(xformMat * sm::float4(objectRadius, 0.0F, 0.0F, 1.0F)) - worldCenter;
    const sm::float3 axisY =
        static_cast<sm::float3>(xformMat * sm::float4(0.0F, objectRadius, 0.0F, 1.0F)) - worldCenter;
    const sm::float3 axisZ =
        static_cast<sm::float3>(xformMat * sm::float4(0.0F, 0.0F, objectRadius, 1.0F)) - worldCenter;
    packed.axisLengths = sm::float3{sm::length(axisX), sm::length(axisY), sm::length(axisZ)};

    const float maxAxis = std::max({packed.axisLengths.x, packed.axisLengths.y, packed.axisLengths.z});
    const float minAxis = std::min({packed.axisLengths.x, packed.axisLengths.y, packed.axisLengths.z});
    if (maxAxis <= 0.0F) {
        return packed; // degenerate (zero-scale) sphere — radius stays 0, nothing appended
    }
    // Uniform ⇔ the axes agree, and then any of them is THE radius.  A genuine ellipsoid is
    // NOT approximated: neither the object-space sphere intersection nor the uniform-cone NEE
    // sampler models one, so substituting an enclosing sphere would hand back a lit point, a
    // normal and a shadow-ray tMax belonging to different geometry than the traced surface.
    // The emitter is skipped instead, and the caller reports it as unsupported authoring.
    packed.uniform = (maxAxis - minAxis) <= kSphereAxisTolerance * maxAxis;
    packed.radius = maxAxis;
    if (!packed.uniform) {
        return packed; // ellipsoid — unsupported, nothing appended
    }

    packed.entry = GpuEmissiveTriangle{
        .v0_area = sm::float4(worldCenter, packed.radius),
        .edge1_emitR = sm::float4(0.0F, 0.0F, 0.0F, emission.r),
        .edge2_emitG = sm::float4(0.0F, 0.0F, 0.0F, emission.g),
        .normal_emitB = sm::float4(0.0F, 0.0F, 0.0F, emission.b),
        .kind_pad = sm::uint4{static_cast<std::uint32_t>(EmissiveKind::Sphere), 0u, 0u, 0u},
    };
    // Power for power-proportional NEE selection: total surface area × luminance(Le),
    // mirroring the triangle's area × luminance(Le) (Rec.2020).  The 4π is NOT a free
    // constant: the CDF is normalized across the whole emitter set, so any scene holding
    // both spheres and triangles compares the two expressions directly and dropping 4π
    // would under-select every sphere by 4π× relative to its true emitted power.
    const float sphereArea = 4.0F * std::numbers::pi_v<float> * packed.radius * packed.radius;
    packed.power = sphereArea * std::max(emissionLuminance(emission), 0.0F);

    data.triangles.push_back(packed.entry);
    data.power.push_back(packed.power);
    packed.appended = true;
    return packed;
}

EmissiveData buildEmissiveData(const std::vector<std::unique_ptr<Geometry>>& meshes,
                               const std::vector<InstanceRecord>& instances,
                               const std::vector<Material>& materials,
                               const std::vector<GpuMaterial>& gpuMaterials) {
    EmissiveData result;

    // (mesh, material) pairs already reported as an unsupported ellipsoid.  Placing one
    // stretched emitter N times is a single authoring mistake, not N of them.
    std::set<std::pair<std::uint32_t, std::uint32_t>> warnedEllipsoids;

    for (std::size_t instIdx = 0; instIdx < instances.size(); ++instIdx) {
        const InstanceRecord& inst = instances[instIdx];
        if (inst.meshIndex >= meshes.size()) {
            continue;
        }
        if (inst.materialIndex >= materials.size() || !materials[inst.materialIndex].emissiveAsLightSource()) {
            continue;
        }
        const GpuMaterial& gpuMat = gpuMaterials[inst.materialIndex];
        if (gpuMat.emissionColorLum.w <= 0.0F) { // NOLINT(cppcoreguidelines-pro-type-union-access)
            continue;
        }
        const Geometry* geom = meshes[inst.meshIndex].get();

        // The mesh lives in object space; the instance transform places it.
        const sm::float4x4 xformMat = inst.xform.matrix();
        const sm::float3 emission = static_cast<sm::float3>(gpuMat.emissionColorLum) *
                                    gpuMat.emissionColorLum.w; // NOLINT(cppcoreguidelines-pro-type-union-access)

        if (const auto* mesh = dynamic_cast<const TriangleMesh*>(geom)) {
            const auto& verts = mesh->data().vertices;
            const auto& idxBuf = mesh->data().indices;
            if (verts.empty() || idxBuf.empty()) {
                continue;
            }

            const std::uint32_t triCount = static_cast<std::uint32_t>(idxBuf.size() / 3);
            for (std::uint32_t t = 0; t < triCount; ++t) {
                const sm::float3 lv0 = verts[idxBuf[t * 3 + 0]].position;
                const sm::float3 lv1 = verts[idxBuf[t * 3 + 1]].position;
                const sm::float3 lv2 = verts[idxBuf[t * 3 + 2]].position;

                const sm::float3 wv0 = static_cast<sm::float3>(xformMat * sm::float4(lv0, 1.0F));
                const sm::float3 wv1 = static_cast<sm::float3>(xformMat * sm::float4(lv1, 1.0F));
                const sm::float3 wv2 = static_cast<sm::float3>(xformMat * sm::float4(lv2, 1.0F));

                const sm::float3 edge1 = wv1 - wv0;
                const sm::float3 edge2 = wv2 - wv0;
                const sm::float3 cross = sm::cross(edge1, edge2);
                const float area = 0.5F * sm::length(cross);

                if (area <= 1.0e-6F) {
                    continue; // skip degenerate triangles
                }
                const sm::float3 normal = cross / (2.0F * area); // normalize: cross/|cross|

                result.triangles.push_back(GpuEmissiveTriangle{
                    .v0_area = sm::float4(wv0, area),
                    .edge1_emitR = sm::float4(edge1, emission.r),
                    .edge2_emitG = sm::float4(edge2, emission.g),
                    .normal_emitB = sm::float4(normal, emission.b),
                    .kind_pad = sm::uint4{static_cast<std::uint32_t>(EmissiveKind::Triangle), 0u, 0u, 0u},
                });
                // Power for power-proportional NEE selection: area × luminance(Le) (Rec.2020).
                result.power.push_back(area * std::max(emissionLuminance(emission), 0.0F));
            }
        } else if (const auto* sphere = dynamic_cast<const Sphere*>(geom)) {
            const SphereEmissive packed = appendSphereEmissive(result, sphere->radius(), xformMat, emission);
            if (packed.radius <= 0.0F) {
                continue; // degenerate (zero-scale) sphere — nothing was appended
            }
            if (!packed.uniform && warnedEllipsoids.emplace(inst.meshIndex, inst.materialIndex).second) {
                Logger::warn("Emissive analytic sphere has a non-uniform instance scale "
                             "(instance {}, mesh {}, material {}): transformed radii "
                             "({:.6g}, {:.6g}, {:.6g}) describe an ellipsoid, which neither "
                             "the object-space sphere intersection nor the uniform-cone NEE "
                             "sampler models. The emitter is SKIPPED (no next-event "
                             "estimation; it is still visible to BSDF paths). Use a uniform "
                             "scale for emissive spheres.",
                             instIdx,
                             inst.meshIndex,
                             inst.materialIndex,
                             packed.axisLengths.x,
                             packed.axisLengths.y,
                             packed.axisLengths.z);
            }
        }
    }

    return result;
}

std::vector<float> buildEmissiveCdf(const std::vector<float>& emissivePower) {
    std::vector<float> cdf;
    cdf.reserve(emissivePower.size());

    double totalPower = 0.0;
    for (const float p : emissivePower) {
        totalPower += static_cast<double>(p);
    }
    if (totalPower > 0.0) {
        double running = 0.0;
        for (const float p : emissivePower) {
            running += static_cast<double>(p);
            cdf.push_back(static_cast<float>(running / totalPower));
        }
        if (!cdf.empty()) {
            cdf.back() = 1.0F; // guard against rounding leaving cdf[N-1] < 1
        }
    } else {
        const auto count = static_cast<std::uint32_t>(emissivePower.size());
        for (std::uint32_t i = 0; i < count; ++i) {
            cdf.push_back(static_cast<float>(i + 1) / static_cast<float>(count));
        }
    }
    if (cdf.empty()) {
        cdf.push_back(1.0F); // sentinel — keeps the binding valid when there are no emitters
    }
    return cdf;
}

} // namespace harmonia
