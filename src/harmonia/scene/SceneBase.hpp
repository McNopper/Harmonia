#ifndef HARMONIA_SCENE_SCENEBASE_HPP
#define HARMONIA_SCENE_SCENEBASE_HPP

#include <volk/volk.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/CommandPool.hpp"
#include "harmonia/scene/Geometry.hpp"
#include "harmonia/scene/ISceneBuilder.hpp"
#include "harmonia/scene/Light.hpp"
#include "harmonia/scene/Material.hpp"
#include "harmonia/scene/Mesh.hpp"
#include "harmonia/scene/Texture.hpp"

namespace harmonia {

/// Non-renderer-specific base for both Hyperion::Scene and Theia::Scene.
///
/// Provides concrete, identical implementations of the operations that do not
/// depend on the GPU layout (addMaterial / addTexture / addLight / addInstance /
/// addMesh / build) and owns the corresponding storage vectors.  Renderer-specific
/// work (addSphereMesh, buildSceneBuffers, buildTlas) is deferred to the derived
/// class via the protected virtual hooks below.
///
/// Storage follows the mesh/instance split: `m_meshes` holds unique geometry
/// (each owning one BLAS) and `m_instances` holds the placements (mesh index +
/// transform + material).  N instances of one mesh share that mesh's BLAS.
class SceneBase : public ISceneBuilder {
  public:
    // ── ISceneBuilder (partial) ──────────────────────────────────────────────

    /// Appends @p mat to the material list and returns its index.
    [[nodiscard]] std::uint32_t addMaterial(Material&& mat) override;

    /// Appends @p texture to the bindless-texture list and returns its index.
    [[nodiscard]] std::uint32_t addTexture(Texture&& texture) override;

    /// Records an instance placement (mesh index + transform + material). Shared
    /// because it is pure bookkeeping — no GPU work until build().
    [[nodiscard]] std::uint32_t
    addInstance(std::uint32_t meshIndex, const Xform& xform, std::uint32_t materialIdx) override;

    /// Registers a unique triangle mesh (object space). Shared because it is
    /// identical across renderers (a plain TriangleMesh upload).
    [[nodiscard]] std::uint32_t
    addMesh(const DeviceContext& ctx, const CommandPool& pool, MeshData&& data, std::string_view name = "") override;

    // ── Additional scene population ─────────────────────────────────────────

    /// Appends @p light and returns its index.  Must be called before build().
    std::uint32_t addLight(std::unique_ptr<Light> light);

    // ── Build (shared orchestration) ────────────────────────────────────────

    /// Uploads scene buffers (buildSceneBuffers), builds one BLAS per unique mesh,
    /// then builds the TLAS (buildTlas). Shared because this orchestration and the
    /// per-mesh BLAS loop are identical across renderers; only the two virtual hooks
    /// diverge. Returns VK_ERROR_INITIALIZATION_FAILED when no instances were added.
    VkResult build(const DeviceContext& ctx, const CommandPool& pool);

    // ── Accessors (read-only) ────────────────────────────────────────────────

    [[nodiscard]] const std::vector<Texture>& textures() const noexcept { return m_textures; }
    [[nodiscard]] const std::vector<std::unique_ptr<Geometry>>& meshes() const noexcept { return m_meshes; }
    [[nodiscard]] const std::vector<InstanceRecord>& instances() const noexcept { return m_instances; }

  protected:
    /// Renderer-specific GPU buffer upload (vertices/indices/materials/emissive/…).
    /// Diverges: Hyperion lays out an index buffer + analytic spheres; Theia builds
    /// meshlets + per-instance visibility masks.
    virtual VkResult buildSceneBuffers(const DeviceContext& ctx, const CommandPool& pool) = 0;

    /// Renderer-specific TLAS build. Diverges: Theia stamps per-instance visibility
    /// masks; Hyperion uses the default mask.
    virtual VkResult buildTlas(const DeviceContext& ctx, const CommandPool& pool) = 0;

    std::vector<Material> m_materials;
    std::vector<std::unique_ptr<Geometry>> m_meshes; ///< unique meshes (one BLAS each)
    std::vector<InstanceRecord> m_instances;         ///< placements referencing m_meshes
    std::vector<std::unique_ptr<Light>> m_lights;
    std::vector<Texture> m_textures;
};

} // namespace harmonia
#endif // HARMONIA_SCENE_SCENEBASE_HPP
