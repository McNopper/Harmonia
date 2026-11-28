#include "harmonia/scene/MaterialLibrary.hpp"

#include <algorithm>
#include <cstdint>

#include "aether/format/MaterialLibrary.hpp"
#include "aether/types/MaterialDesc.hpp"
#include "harmonia/GpuTypes.hpp"
#include "harmonia/core/Logger.hpp"
#include "harmonia/utils/ColorSpace.hpp"

namespace harmonia::scene {

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
    case aether::TextureColorSpace::LinAcesCgScene:
        return TextureColorSpace::LinAcesCgScene;
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
[[nodiscard]] Material buildMaterial(const aether::MaterialDesc& p,
                                     harmonia::utils::ColorSpace::WorkingColorSpace workingSpace) {
    auto sourceSpace = harmonia::utils::ColorSpace::WorkingColorSpace::LinRec709;
    switch (p.inputColorSpace) {
    case aether::MaterialColorSpace::LinRec709:
        sourceSpace = harmonia::utils::ColorSpace::WorkingColorSpace::LinRec709;
        break;
    case aether::MaterialColorSpace::LinRec2020:
        sourceSpace = harmonia::utils::ColorSpace::WorkingColorSpace::LinRec2020;
        break;
    case aether::MaterialColorSpace::LinAcesCg:
        sourceSpace = harmonia::utils::ColorSpace::WorkingColorSpace::LinAcesCg;
        break;
    }
    const auto cc = [sourceSpace, workingSpace](sm::float3 c) {
        return harmonia::utils::ColorSpace::convertLinearRgb(c, sourceSpace, workingSpace);
    };

    GpuMaterial g{};

    // Base layer
    g.baseColorWeight = sm::float4(cc(p.base_color), p.base_weight);
    const bool texturedEmission = !p.map_emission_color.empty() || !p.map_emission_luminance.empty();
    const bool hitOnlyTexturedEmission = texturedEmission && p.emission_as_light_source;
    g.baseMetalnessDiffRough =
        sm::float4{p.base_metalness, p.base_diffuse_roughness, hitOnlyTexturedEmission ? 1.0f : 0.0f, 0.0f};

    // Specular
    g.specularColorWeight = sm::float4(cc(p.specular_color), p.specular_weight);
    g.specularRoughAnisoIor =
        sm::float4{p.specular_roughness, p.specular_roughness_anisotropy, std::max(p.specular_ior, 1.0f), 0.0f};

    // Transmission
    g.transmissionColorWeight = sm::float4(cc(p.transmission_color), p.transmission_weight);
    g.transmissionParams = sm::float4{p.transmission_depth,
                                      p.specular_roughness,
                                      p.transmission_dispersion_scale,
                                      std::max(p.transmission_dispersion_abbe_number, 1.0f)};
    g.transmissionScatter = sm::float4(p.transmission_scatter, p.transmission_scatter_anisotropy);

    // Subsurface: GpuMaterial packs the per-channel scale in xyz, scalar radius in w.
    g.subsurfaceColorWeight = sm::float4(cc(p.subsurface_color), p.subsurface_weight);
    g.subsurfaceRadiusScale = sm::float4(p.subsurface_radius_scale, p.subsurface_radius);

    // Texture indices (filled by SceneLoader later; sentinel = kNoTexture)
    g.textureIndices = sm::uint4{kNoTexture, kNoTexture, kNoTexture, kNoTexture};
    g.textureIndices2 = sm::uint4{kNoTexture, kNoTexture, kNoTexture, kNoTexture};
    g.textureIndices3 = sm::uint4{kNoTexture, kNoTexture, kNoTexture, kNoTexture};
    g.textureIndices4 = sm::uint4{kNoTexture, kNoTexture, kNoTexture, kNoTexture};

    // Thin film
    g.thinFilmParams = sm::float4{p.thin_film_thickness, std::max(p.thin_film_ior, 1.0f), p.thin_film_weight, 0.0f};

    // Coat
    g.coatColorWeight = sm::float4(cc(p.coat_color), p.coat_weight);
    g.coatRoughAnisoIorDark = sm::float4{
        p.coat_roughness, p.coat_roughness_anisotropy, std::max(p.coat_ior, 1.0f), sm::saturate(p.coat_darkening)};

    // Fuzz / sheen
    g.fuzzColorWeight = sm::float4(cc(p.fuzz_color), p.fuzz_weight);
    g.fuzzRoughPad = sm::float4{sm::saturate(p.fuzz_roughness), 0.0f, 0.0f, 0.0f};

    // Emission
    // A mapped emission luminance is a normalized texture times an authored scale. Legacy
    // converted libraries omitted the scale and the shader replaced the zero default, so retain
    // that behavior with an implicit scale of one. Unmapped materials use their scalar directly.
    const float emissionLuminance =
        !p.map_emission_luminance.empty() && p.emission_luminance == 0.0F ? 1.0F : p.emission_luminance;
    g.emissionColorLum = sm::float4(cc(p.emission_color), std::max(emissionLuminance, 0.0f));

    // The glass flag advertises the exact full-dielectric R+T path to SMS. Fractional transmission
    // remains a layered substrate mixture; computeLobeWeights resolves the same condition again
    // after texture maps replace the scalar weight/metalness.
    const float flags = (p.transmission_weight >= 1.0f && p.base_metalness <= 0.0f) ? 2.0f : 0.0f;
    g.opacityFlagsPad = sm::float4{
        sm::saturate(p.geometry_opacity), flags, p.subsurface_scatter_anisotropy, p.geometry_thin_walled ? 1.0f : 0.0f};

    // Textured emission has no untextured power CDF. Keep it out of constant-radiance NEE and
    // mark it for unbiased BSDF-hit emission at every depth instead (evalEmissionGuarded).
    return Material::fromGpu(g, p.emission_as_light_source && !texturedEmission);
}

[[nodiscard]] MaterialLibrary::MaterialTextureRef toRef(const aether::TextureRef& r,
                                                        const std::filesystem::path& libraryDir) {
    std::filesystem::path texturePath(r.path);
    if (!texturePath.empty() && texturePath.is_relative()) {
        // A library in a subdirectory naturally refers to ../textures. Prefer that declaring
        // library's directory when the file exists there; otherwise leave the path relative so
        // SceneLoader's established scene-directory then assets-directory fallback still applies.
        const std::filesystem::path local = (libraryDir / texturePath).lexically_normal();
        std::error_code ec;
        if (std::filesystem::exists(local, ec)) {
            texturePath = local;
        }
    }
    return MaterialLibrary::MaterialTextureRef{texturePath.string(), toHarmonia(r.colorSpace)};
}

} // namespace

// ── MaterialLibrary ───────────────────────────────────────────────────────

bool MaterialLibrary::load(const std::filesystem::path& path,
                           harmonia::utils::ColorSpace::WorkingColorSpace workingSpace) {
    aether::MaterialLibrary parsed;
    if (!parsed.load(path)) {
        core::Logger::error("MaterialLibrary: cannot open '{}'", path.string());
        return false;
    }

    const std::filesystem::path libraryDir = std::filesystem::absolute(path).lexically_normal().parent_path();
    for (const auto& [name, desc] : parsed.materials()) {
        m_materials.insert_or_assign(name, buildMaterial(desc, workingSpace));

        MaterialTextureRefs refs;
        refs.base_color = toRef(desc.map_base_color, libraryDir);
        refs.normal = toRef(desc.map_normal, libraryDir);
        refs.orm = toRef(desc.map_orm, libraryDir);
        refs.emission = toRef(desc.map_emission_color, libraryDir);
        refs.coat_normal = toRef(desc.map_coat_normal, libraryDir);
        refs.tangent = toRef(desc.map_tangent, libraryDir);
        refs.coat_tangent = toRef(desc.map_coat_tangent, libraryDir);
        refs.opacity = toRef(desc.map_opacity, libraryDir);
        refs.coat_weight = toRef(desc.map_coat_weight, libraryDir);
        refs.transmission_weight = toRef(desc.map_transmission_weight, libraryDir);
        refs.subsurface_color = toRef(desc.map_subsurface_color, libraryDir);
        refs.subsurface_weight = toRef(desc.map_subsurface_weight, libraryDir);
        refs.transmission_color = toRef(desc.map_transmission_color, libraryDir);
        refs.transmission_scatter = toRef(desc.map_transmission_scatter, libraryDir);
        refs.emission_luminance = toRef(desc.map_emission_luminance, libraryDir);
        refs.fuzz_weight = toRef(desc.map_fuzz_weight, libraryDir);
        m_textureRefs.insert_or_assign(name, std::move(refs));
    }

    core::Logger::info(
        "MaterialLibrary: loaded {} material(s) from '{}'", m_materials.size(), path.filename().string());
    return true;
}

std::optional<Material> MaterialLibrary::get(const std::string& name) const {
    const auto it = m_materials.find(name);
    return it != m_materials.end() ? std::optional{it->second} : std::nullopt;
}

Material MaterialLibrary::getOrDefault(const std::string& name) const {
    return get(name).value_or(Material::diffuse(sm::float3{0.8f, 0.8f, 0.8f}));
}

std::optional<MaterialLibrary::MaterialTextureRefs> MaterialLibrary::textureRefs(const std::string& name) const {
    const auto it = m_textureRefs.find(name);
    return it != m_textureRefs.end() ? std::optional{it->second} : std::nullopt;
}

void MaterialLibrary::patchTextureIndex(const std::string& name, std::uint32_t slot, std::uint32_t idx) {
    const auto it = m_materials.find(name);
    if (it != m_materials.end())
        it->second.setTextureIndex(slot, idx);
}

} // namespace harmonia::scene
