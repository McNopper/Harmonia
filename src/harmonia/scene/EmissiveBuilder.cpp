#include "harmonia/scene/EmissiveBuilder.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <slang-math/slang-math.hpp>

#include "harmonia/scene/Geometry.hpp"
#include "harmonia/scene/Mesh.hpp"

namespace harmonia {

EmissiveData buildEmissiveData(const std::vector<std::unique_ptr<Geometry>>& meshes,
                               const std::vector<InstanceRecord>& instances,
                               const std::vector<Material>& materials,
                               const std::vector<GpuMaterial>& gpuMaterials) {
    EmissiveData result;

    for (const InstanceRecord& inst : instances) {
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
        const auto* mesh = dynamic_cast<const TriangleMesh*>(meshes[inst.meshIndex].get());
        if (mesh == nullptr) {
            continue; // spheres not supported for NEE yet
        }
        const auto& verts = mesh->data().vertices;
        const auto& idxBuf = mesh->data().indices;
        if (verts.empty() || idxBuf.empty()) {
            continue;
        }

        // The mesh lives in object space; the instance transform places it.
        const sm::float4x4 xformMat = inst.xform.matrix();
        const sm::float3 emission = static_cast<sm::float3>(gpuMat.emissionColorLum) *
                                    gpuMat.emissionColorLum.w; // NOLINT(cppcoreguidelines-pro-type-union-access)

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
            });
            // Power for power-proportional NEE selection: area × luminance(Le) (Rec.2020).
            const float lumLe = 0.2627F * emission.r + 0.6780F * emission.g + 0.0593F * emission.b;
            result.power.push_back(area * std::max(lumLe, 0.0F));
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
