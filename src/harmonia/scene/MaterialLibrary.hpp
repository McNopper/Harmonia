#ifndef HARMONIA_SCENE_MATERIALLIBRARY_HPP
#define HARMONIA_SCENE_MATERIALLIBRARY_HPP

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

#include "harmonia/scene/Material.hpp"
#include "harmonia/scene/Texture.hpp"

/// Loads an OpenPBR `.materials.toml` material library into GPU-ready Harmonia materials.
///
/// Parsing of the `.materials.toml` TOML format is owned by Aether (`aether::MaterialLibrary`,
/// producing renderer-agnostic `aether::MaterialDesc`). This class is the GPU-side
/// half shared by every renderer: it drives the Aether parser, packs each parsed
/// `MaterialDesc` into a `GpuMaterial` (converting color values from the
/// material's declared linear input color space to the scene's working color
/// space), records the per-material texture references, and lets the caller
/// patch bindless texture indices once the textures have been uploaded.
///
/// The `.materials.toml` format, OpenPBR keyword set and texture color-space
/// handling are all documented on `aether::MaterialLibrary`.
class MaterialLibrary {
  public:
    /// Reference to one texture map: file path + source color space.
    struct MaterialTextureRef {
        std::string path;
        TextureColorSpace colorSpace = TextureColorSpace::SrgbRec709Scene;
        [[nodiscard]] bool empty() const noexcept { return path.empty(); }
    };

    /// All texture references for one material (one entry per bindless map slot).
    /// Slot order matches GpuMaterial::textureIndices: [0] base_color, [1] normal,
    /// [2] ORM (occlusion/roughness/metalness), [3] emission.
    struct MaterialTextureRefs {
        MaterialTextureRef base_color;
        MaterialTextureRef normal;
        MaterialTextureRef orm;
        MaterialTextureRef emission;
        MaterialTextureRef coat_normal;
        MaterialTextureRef tangent;
        MaterialTextureRef coat_tangent;
    };

    /// Load material definitions from a .materials.toml file, converting color
    /// values into @p workingSpace. Returns false only if the file cannot be opened.
    bool load(const std::filesystem::path& path,
              ColorSpace::WorkingColorSpace workingSpace = ColorSpace::WorkingColorSpace::LinRec2020);

    /// Look up a material by name.  Returns std::nullopt if not found.
    [[nodiscard]] std::optional<Material> get(const std::string& name) const;

    /// Look up a material by name, or return a default diffuse-gray material.
    [[nodiscard]] Material getOrDefault(const std::string& name) const;

    /// Return the texture references recorded for the named material,
    /// or std::nullopt if the material is not found.
    [[nodiscard]] std::optional<MaterialTextureRefs> textureRefs(const std::string& name) const;

    /// Patch the texture index for slot in the stored material.
    /// Call this after the texture has been uploaded to the GPU so that
    /// subsequent getOrDefault() calls return the correct bindless index.
    void patchTextureIndex(const std::string& name, std::uint32_t slot, std::uint32_t idx);

    [[nodiscard]] bool empty() const noexcept { return m_materials.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return m_materials.size(); }

  private:
    std::unordered_map<std::string, Material> m_materials;
    std::unordered_map<std::string, MaterialTextureRefs> m_textureRefs;
};
#endif // HARMONIA_SCENE_MATERIALLIBRARY_HPP
