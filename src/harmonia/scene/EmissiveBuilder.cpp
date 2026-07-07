#include "harmonia/scene/EmissiveBuilder.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <slang-math/slang-math.hpp>

#include "harmonia/scene/Geometry.hpp"
#include "harmonia/scene/Mesh.hpp"

namespace harmonia {

EmissiveData buildEmissiveData(const std::vector<std::unique_ptr<Geometry>>& geometries,
                                const std::vector<EmissiveInstanceInfo>&      instances,
                                const std::vector<Material>&                  materials,
                                const std::vector<GpuMaterial>&               gpuMaterials) {
    EmissiveData result;

    for (size_t i = 0; i < geometries.size(); ++i) {
        const EmissiveInstanceInfo& inst = instances[i];
        if (inst.geometryKind != 0U) {
            continue; // spheres not supported for NEE yet
        }
        if (inst.materialIndex >= materials.size() || !materials[inst.materialIndex].emissiveAsLightSource()) {
            continue;
        }
        const GpuMaterial& gpuMat = gpuMaterials[inst.materialIndex];
        if (gpuMat.emissionColorLum.w <= 0.0F) { // NOLINT(cppcoreguidelines-pro-type-union-access)
            continue;
        }
        const auto* mesh = dynamic_cast<const TriangleMesh*>(geometries[i].get());
        if (mesh == nullptr) {
            continue;
        }
        const auto& verts  = mesh->data().vertices;
        const auto& idxBuf = mesh->data().indices;
        if (verts.empty() || idxBuf.empty()) {
            continue;
        }

        const sm::float4x4 xformMat = geometries[i]->xform.matrix();
        const sm::float3 emission   = static_cast<sm::float3>(gpuMat.emissionColorLum) *
                                      gpuMat.emissionColorLum.w; // NOLINT(cppcoreguidelines-pro-type-union-access)

        const uint32_t triCount = static_cast<uint32_t>(idxBuf.size() / 3);
        for (uint32_t t = 0; t < triCount; ++t) {
            const sm::float3 lv0 = verts[idxBuf[t * 3 + 0]].position;
            const sm::float3 lv1 = verts[idxBuf[t * 3 + 1]].position;
            const sm::float3 lv2 = verts[idxBuf[t * 3 + 2]].position;

            const sm::float3 wv0 = static_cast<sm::float3>(xformMat * sm::float4(lv0, 1.0F));
            const sm::float3 wv1 = static_cast<sm::float3>(xformMat * sm::float4(lv1, 1.0F));
            const sm::float3 wv2 = static_cast<sm::float3>(xformMat * sm::float4(lv2, 1.0F));

            const sm::float3 edge1 = wv1 - wv0;
            const sm::float3 edge2 = wv2 - wv0;
            const sm::float3 cross = sm::cross(edge1, edge2);
            const float area       = 0.5F * sm::length(cross);

            if (area <= 1.0e-6F) {
                continue; // skip degenerate triangles
            }
            const sm::float3 normal = cross / (2.0F * area); // normalize: cross/|cross|

            result.triangles.push_back(GpuEmissiveTriangle{
                .v0_area        = sm::float4(wv0,    area),
                .edge1_emitR    = sm::float4(edge1,  emission.r),
                .edge2_emitG    = sm::float4(edge2,  emission.g),
                .normal_emitB   = sm::float4(normal, emission.b),
            });
            // Power for power-proportional NEE selection: area × luminance(Le) (Rec.2020).
            const float lumLe = 0.2627F * emission.r + 0.6780F * emission.g + 0.0593F * emission.b;
            result.power.push_back(area * std::max(lumLe, 0.0F));
        }
    }

    return result;
}

} // namespace harmonia
