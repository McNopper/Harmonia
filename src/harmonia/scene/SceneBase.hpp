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
/// Provides concrete, identical implementations of the operations that do not
/// depend on the GPU layout (addMaterial / addTexture / addLight / addInstance)
/// and owns the corresponding storage vectors.  Renderer-specific methods
/// (addMesh, addSphereMesh, buildSceneBuffers, buildTlas, ...) remain in the
/// derived class.
///
/// Storage follows the mesh/instance split: `m_meshes` holds unique geometry
/// (each owning one BLAS) and `m_instances` holds the placements (mesh index +
/// transform + material).  N instances of one mesh share that mesh's BLAS.
class SceneBase : public ISceneBuilder {
  public:
    // ── ISceneBuilder (partial) ──────────────────────────────────────────────

    /// Appends @p mat to the material list and returns its index.
    [[nodiscard]] uint32_t addMaterial(Material&& mat) override;

    /// Appends @p texture to the bindless-texture list and returns its index.
    [[nodiscard]] uint32_t addTexture(Texture&& texture) override;

    /// Records an instance placement (mesh index + transform + material). Shared
    /// because it is pure bookkeeping — no GPU work until build().
    [[nodiscard]] uint32_t addInstance(uint32_t meshIndex, const Xform& xform, uint32_t materialIdx) override;

    // ── Additional scene population ─────────────────────────────────────────

    /// Appends @p light and returns its index.  Must be called before build().
    uint32_t addLight(std::unique_ptr<Light> light);

    // ── Accessors (read-only) ────────────────────────────────────────────────

    [[nodiscard]] const std::vector<Texture>& textures() const noexcept { return m_textures; }
    [[nodiscard]] const std::vector<std::unique_ptr<Geometry>>& meshes() const noexcept { return m_meshes; }
    [[nodiscard]] const std::vector<InstanceRecord>& instances() const noexcept { return m_instances; }

  protected:
    std::vector<Material>                  m_materials;
    std::vector<std::unique_ptr<Geometry>> m_meshes;   ///< unique meshes (one BLAS each)
    std::vector<InstanceRecord>            m_instances; ///< placements referencing m_meshes
    std::vector<std::unique_ptr<Light>>    m_lights;
    std::vector<Texture>                   m_textures;
};

} // namespace harmonia
