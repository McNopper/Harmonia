#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include "harmonia/scene/MaterialLibrary.hpp"

#include <glm/glm.hpp>

#include <algorithm>

#include "aether/format/MaterialLibrary.hpp"
#include "aether/types/MaterialDesc.hpp"
#include "harmonia/GpuTypes.hpp"
#include "harmonia/core/Logger.hpp"
#include "harmonia/utils/ColorSpace.hpp"

namespace {

/// Map Aether's texture color-space enum to Harmonia's (identical value order).
[[nodiscard]] TextureColorSpace toHarmonia(aether::TextureColorSpace cs) noexcept {
    switch (cs) {
    case aether::TextureColorSpace::SrgbRec709Scene:
        return TextureColorSpace::SrgbRec709Scene;
    case aether::TextureColorSpace::LinRec709Scene:
        return TextureColorSpace::LinRec709Scene;
    case aether::TextureColorSpace::LinRec2020Scene:
        return TextureColorSpace::LinRec2020Scene;
    case aether::TextureColorSpace::Data:
        break;
    }
    return TextureColorSpace::Data;
}

// ── Build a GpuMaterial from a parsed OpenPBR MaterialDesc ─────────────────
// Color values are converted from the material's declared (linear) input color
// space to the scene's working color space; non-color data (subsurface_radius,
// transmission_scatter) is never color-converted.  All lobes are set directly;
// no threshold-based type select.
// flags: 0 = general layered, 2 = glass/dielectric, 3 = mirror (not set here).
[[nodiscard]] Material buildMaterial(const aether::MaterialDesc& p, ColorSpace::WorkingColorSpace workingSpace) {
    const bool srcRec709 = (p.inputColorSpace == aether::MaterialColorSpace::LinRec709);
    const bool dstRec2020 = (workingSpace == ColorSpace::WorkingColorSpace::LinRec2020);
    const auto cc = [srcRec709, dstRec2020](glm::vec3 c) {
        if (srcRec709 && dstRec2020)
            return ColorSpace::rec709ToRec2020(c);
        if (!srcRec709 && !dstRec2020)
            return ColorSpace::rec2020ToRec709(c);
        return c; // declared space == working space
    };

    GpuMaterial g{};

    // Base layer
    g.baseColorWeight = glm::vec4(cc(p.base_color), p.base_weight);
    g.baseMetalnessDiffRough = glm::vec4(p.base_metalness, p.base_diffuse_roughness, 0.0f, 0.0f);

    // Specular
    g.specularColorWeight = glm::vec4(cc(p.specular_color), p.specular_weight);
    g.specularRoughAnisoIor =
        glm::vec4(p.specular_roughness, p.specular_roughness_anisotropy, std::max(p.specular_ior, 1.0f), 0.0f);

    // Transmission
    g.transmissionColorWeight = glm::vec4(cc(p.transmission_color), p.transmission_weight);
    g.transmissionParams = glm::vec4(p.transmission_depth,
                                     p.specular_roughness,
                                     p.transmission_dispersion_scale,
                                     std::max(p.transmission_dispersion_abbe_number, 1.0f));
    g.transmissionScatter = glm::vec4(p.transmission_scatter, p.transmission_scatter_anisotropy);

    // Subsurface
    g.subsurfaceColorWeight = glm::vec4(cc(p.subsurface_color), p.subsurface_weight);
    g.subsurfaceRadiusScale = glm::vec4(p.subsurface_radius, p.subsurface_scale);

    // Texture indices (filled by SceneLoader later; sentinel = kNoTexture)
    g.textureIndices = glm::uvec4(kNoTexture);
    g.textureIndices2 = glm::uvec4(kNoTexture);

    // Thin film
    g.thinFilmParams = glm::vec4(p.thin_film_thickness, std::max(p.thin_film_ior, 1.0f), p.thin_film_weight, 0.0f);

    // Coat
    g.coatColorWeight = glm::vec4(cc(p.coat_color), p.coat_weight);
    g.coatRoughAnisoIorDark = glm::vec4(p.coat_roughness,
                                        p.coat_roughness_anisotropy,
                                        std::max(p.coat_ior, 1.0f),
                                        std::clamp(p.coat_darkening, 0.0f, 1.0f));

    // Fuzz / sheen
    g.fuzzColorWeight = glm::vec4(cc(p.fuzz_color), p.fuzz_weight);
    g.fuzzRoughPad = glm::vec4(std::clamp(p.fuzz_roughness, 0.0f, 1.0f), 0.0f, 0.0f, 0.0f);

    // Emission
    g.emissionColorLum = glm::vec4(cc(p.emission_color), std::max(p.emission_luminance, 0.0f));

    // Opacity + flags: glass mode enables Fresnel split in sampleBSDF.
    const float flags = (p.transmission_weight >= 0.5f && p.base_metalness < 0.5f) ? 2.0f : 0.0f;
    g.opacityFlagsPad = glm::vec4(std::clamp(p.opacity, 0.0f, 1.0f),
                                  flags,
                                  p.subsurface_scatter_anisotropy,
                                  p.thin_walled ? 1.0f : 0.0f);

    return Material::fromGpu(g);
}

[[nodiscard]] MaterialLibrary::MaterialTextureRef toRef(const aether::TextureRef& r) {
    return MaterialLibrary::MaterialTextureRef{r.path, toHarmonia(r.colorSpace)};
}

} // namespace

// ── MaterialLibrary ───────────────────────────────────────────────────────

bool MaterialLibrary::load(const std::filesystem::path& path, ColorSpace::WorkingColorSpace workingSpace) {
    aether::MaterialLibrary parsed;
    if (!parsed.load(path)) {
        Logger::error("MaterialLibrary: cannot open '{}'", path.string());
        return false;
    }

    for (const auto& [name, desc] : parsed.materials()) {
        m_materials.insert_or_assign(name, buildMaterial(desc, workingSpace));

        MaterialTextureRefs refs;
        refs.base_color = toRef(desc.map_base_color);
        refs.normal = toRef(desc.map_normal);
        refs.orm = toRef(desc.map_orm);
        refs.emission = toRef(desc.map_emission_color);
        refs.coat_normal = toRef(desc.map_coat_normal);
        refs.tangent = toRef(desc.map_tangent);
        refs.coat_tangent = toRef(desc.map_coat_tangent);
        m_textureRefs.insert_or_assign(name, std::move(refs));
    }

    Logger::info("MaterialLibrary: loaded {} material(s) from '{}'", m_materials.size(), path.filename().string());
    return true;
}

std::optional<Material> MaterialLibrary::get(const std::string& name) const {
    const auto it = m_materials.find(name);
    return it != m_materials.end() ? std::optional{it->second} : std::nullopt;
}

Material MaterialLibrary::getOrDefault(const std::string& name) const {
    return get(name).value_or(Material::diffuse(glm::vec3(0.8f)));
}

std::optional<MaterialLibrary::MaterialTextureRefs> MaterialLibrary::textureRefs(const std::string& name) const {
    const auto it = m_textureRefs.find(name);
    return it != m_textureRefs.end() ? std::optional{it->second} : std::nullopt;
}

void MaterialLibrary::patchTextureIndex(const std::string& name, uint32_t slot, uint32_t idx) {
    const auto it = m_materials.find(name);
    if (it != m_materials.end())
        it->second.setTextureIndex(slot, idx);
}
