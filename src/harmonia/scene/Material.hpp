#ifndef HARMONIA_SCENE_MATERIAL_HPP
#define HARMONIA_SCENE_MATERIAL_HPP

#include <algorithm>
#include <cstdint>
#include <slang-math/slang-math.hpp>

#include "harmonia/GpuTypes.hpp"

namespace harmonia {

class Material {
  public:
    [[nodiscard]] static Material diffuse(sm::float3 color, float roughness = 1.0f, float specIor = 1.5f) {
        Material material;
        material.m_gpu.baseColorWeight = sm::float4(sm::max(color, sm::float3{0.0f, 0.0f, 0.0f}), 1.0f);
        material.m_gpu.baseMetalnessDiffRough = sm::float4{0.0f, std::clamp(roughness, 0.0f, 1.0f), 0.0f, 0.0f};
        material.m_gpu.specularColorWeight = sm::float4{1.0f, 1.0f, 1.0f, 1.0f};
        material.m_gpu.specularRoughAnisoIor =
            sm::float4{std::clamp(roughness, 0.0f, 1.0f), 0.0f, std::max(specIor, 1.0f), 0.0f};
        material.m_gpu.transmissionColorWeight = sm::float4{1.0f, 1.0f, 1.0f, 0.0f};
        material.m_gpu.subsurfaceRadiusScale = sm::float4{1.0f, 1.0f, 1.0f, 1.0f};
        material.m_gpu.textureIndices = sm::uint4{kNoTexture, kNoTexture, kNoTexture, kNoTexture};
        material.m_gpu.textureIndices2 = sm::uint4{kNoTexture, kNoTexture, kNoTexture, kNoTexture};
        material.m_gpu.coatColorWeight = sm::float4{1.0f, 1.0f, 1.0f, 0.0f};
        material.m_gpu.coatRoughAnisoIorDark = sm::float4{0.0f, 0.0f, 1.6f, 0.0f};
        material.m_gpu.fuzzColorWeight = sm::float4{1.0f, 1.0f, 1.0f, 0.0f};
        material.m_gpu.fuzzRoughPad = sm::float4{0.5f, 0.0f, 0.0f, 0.0f};
        material.m_gpu.opacityFlagsPad = sm::float4{1.0f, 0.0f, 0.0f, 0.0f};
        return material;
    }

    [[nodiscard]] static Material metal(sm::float3 color, float roughness = 0.0f) {
        Material material = diffuse(color, roughness, 1.5f);
        material.m_gpu.baseMetalnessDiffRough.x = 1.0f;
        material.m_gpu.baseMetalnessDiffRough.y = 0.0f;
        material.m_gpu.specularColorWeight = sm::float4(sm::max(color, sm::float3{0.0f, 0.0f, 0.0f}), 1.0f);
        material.m_gpu.specularRoughAnisoIor.x = std::clamp(roughness, 0.0f, 1.0f);
        return material;
    }

    [[nodiscard]] static Material mirror(sm::float3 color = {0.99f, 0.99f, 0.99f}) {
        Material material = metal(color, 0.0f);
        material.m_gpu.specularRoughAnisoIor.x = 0.0f;
        material.m_gpu.opacityFlagsPad = sm::float4{1.0f, 3.0f, 0.0f, 0.0f}; // flags=3 → mirror (delta reflect)
        return material;
    }

    [[nodiscard]] static Material glass(float ior = 1.52f, float roughness = 0.0f) {
        Material material = diffuse(sm::float3{1.0f, 1.0f, 1.0f}, roughness, ior);
        material.m_gpu.baseColorWeight = sm::float4{1.0f, 1.0f, 1.0f, 1.0f};
        material.m_gpu.baseMetalnessDiffRough = sm::float4{0.0f, 0.0f, 0.0f, 0.0f};
        material.m_gpu.specularColorWeight = sm::float4{1.0f, 1.0f, 1.0f, 1.0f};
        material.m_gpu.specularRoughAnisoIor =
            sm::float4{std::clamp(roughness, 0.0f, 1.0f), 0.0f, std::max(ior, 1.0f), 0.0f};
        material.m_gpu.transmissionColorWeight = sm::float4{1.0f, 1.0f, 1.0f, 1.0f};
        material.m_gpu.transmissionParams = sm::float4{0.0f, std::clamp(roughness, 0.0f, 1.0f), 0.0f, 0.0f};
        material.m_gpu.opacityFlagsPad = sm::float4{1.0f, 2.0f, 0.0f, 0.0f}; // opacity=1, flags=2 → glass
        return material;
    }

    [[nodiscard]] static Material emissive(sm::float3 color, float luminanceNits) {
        Material material = diffuse(color, 1.0f, 1.5f);
        material.m_gpu.emissionColorLum =
            sm::float4(sm::max(color, sm::float3{0.0f, 0.0f, 0.0f}), std::max(luminanceNits, 0.0f));
        return material;
    }

    /// Construct a Material wrapping an already-populated GpuMaterial.
    [[nodiscard]] static Material fromGpu(const GpuMaterial& g, bool emissiveAsLightSource = true) {
        Material m;
        m.m_gpu = g;
        m.m_emissiveAsLightSource = emissiveAsLightSource;
        return m;
    }

    Material& baseWeight(float w) {
        m_gpu.baseColorWeight.w = std::clamp(w, 0.0f, 1.0f);
        return *this;
    }

    Material& coat(float w, sm::float3 color = {1.0f, 1.0f, 1.0f}, float ior = 1.6f, float rough = 0.0f) {
        m_gpu.coatColorWeight = sm::float4(sm::max(color, sm::float3{0.0f, 0.0f, 0.0f}), std::clamp(w, 0.0f, 1.0f));
        m_gpu.coatRoughAnisoIorDark = sm::float4{std::clamp(rough, 0.0f, 1.0f), 0.0f, std::max(ior, 1.0f), 0.0f};
        return *this;
    }

    Material& fuzz(float w, sm::float3 color = {1.0f, 1.0f, 1.0f}, float rough = 0.5f) {
        m_gpu.fuzzColorWeight = sm::float4(sm::max(color, sm::float3{0.0f, 0.0f, 0.0f}), std::clamp(w, 0.0f, 1.0f));
        m_gpu.fuzzRoughPad = sm::float4{std::clamp(rough, 0.0f, 1.0f), 0.0f, 0.0f, 0.0f};
        return *this;
    }

    Material& subsurface(float w, sm::float3 color, sm::float3 radius, float scale = 1.0f) {
        m_gpu.subsurfaceColorWeight =
            sm::float4(sm::max(color, sm::float3{0.0f, 0.0f, 0.0f}), std::clamp(w, 0.0f, 1.0f));
        m_gpu.subsurfaceRadiusScale = sm::float4(sm::max(radius, sm::float3{0.0f, 0.0f, 0.0f}), std::max(scale, 0.0f));
        return *this;
    }

    Material& opacity(float o) {
        m_gpu.opacityFlagsPad.x = std::clamp(o, 0.0f, 1.0f);
        // flags (y) intentionally NOT touched — caller must set it explicitly
        return *this;
    }

    /// Set a bindless texture index for a given map slot.
    /// Slots 0-3 → textureIndices [base_color, normal, ORM, emission];
    /// slots 4-6 → textureIndices2 [coat_normal, tangent, coat_tangent];
    /// slot 7 → textureIndices2.w [opacity] (rasterizer alpha-test map).
    void setTextureIndex(std::uint32_t slot, std::uint32_t idx) noexcept {
        switch (slot) {
        case 0:
            m_gpu.textureIndices.x = idx;
            break;
        case 1:
            m_gpu.textureIndices.y = idx;
            break;
        case 2:
            m_gpu.textureIndices.z = idx;
            break;
        case 3:
            m_gpu.textureIndices.w = idx;
            break;
        case 4:
            m_gpu.textureIndices2.x = idx;
            break;
        case 5:
            m_gpu.textureIndices2.y = idx;
            break;
        case 6:
            m_gpu.textureIndices2.z = idx;
            break;
        case 7:
            m_gpu.textureIndices2.w = idx;
            break;
        default:
            break;
        }
    }

    [[nodiscard]] const GpuMaterial& gpu() const noexcept { return m_gpu; }
    [[nodiscard]] bool emissiveAsLightSource() const noexcept { return m_emissiveAsLightSource; }
    void setEmissiveAsLightSource(bool enabled) noexcept { m_emissiveAsLightSource = enabled; }

  private:
    GpuMaterial m_gpu{};
    bool m_emissiveAsLightSource = true;
};

} // namespace harmonia

#endif // HARMONIA_SCENE_MATERIAL_HPP
