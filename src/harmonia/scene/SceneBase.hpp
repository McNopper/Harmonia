#pragma once

#include <memory>
#include <vector>

#include "harmonia/scene/Geometry.hpp"
#include "harmonia/scene/ISceneBuilder.hpp"
#include "harmonia/scene/Light.hpp"
#include "harmonia/scene/Material.hpp"
#include "harmonia/scene/Texture.hpp"

namespace harmonia {

/// Non-renderer-specific base for both Hyperion::Scene and Theia::Scene.
///
/// Provides concrete, identical implementations of the three operations that
/// do not depend on the GPU layout (addMaterial / addTexture / addLight) and
/// owns the corresponding storage vectors.  Renderer-specific methods
/// (addMesh, addSphere, buildSceneBuffers, buildTlas, ...) remain in the
/// derived class.
class SceneBase : public ISceneBuilder {
  public:
    // ── ISceneBuilder (partial) ──────────────────────────────────────────────

    /// Appends @p mat to the material list and returns its index.
    [[nodiscard]] uint32_t addMaterial(Material&& mat) override;

    /// Appends @p texture to the bindless-texture list and returns its index.
    [[nodiscard]] uint32_t addTexture(Texture&& texture) override;

    // ── Additional scene population ─────────────────────────────────────────

    /// Appends @p light and returns its index.  Must be called before build().
    uint32_t addLight(std::unique_ptr<Light> light);

    // ── Accessors (read-only) ────────────────────────────────────────────────

    [[nodiscard]] const std::vector<Texture>& textures() const noexcept { return m_textures; }

  protected:
    std::vector<Material>                  m_materials;
    std::vector<std::unique_ptr<Geometry>> m_geometries;
    std::vector<std::unique_ptr<Light>>    m_lights;
    std::vector<Texture>                   m_textures;
};

} // namespace harmonia
