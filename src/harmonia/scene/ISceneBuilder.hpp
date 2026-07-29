#ifndef HARMONIA_SCENE_ISCENEBUILDER_HPP
#define HARMONIA_SCENE_ISCENEBUILDER_HPP

#include <cstdint>
#include <slang-math/slang-math.hpp>
#include <string_view>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/CommandPool.hpp"
#include "harmonia/scene/Geometry.hpp"
#include "harmonia/scene/Material.hpp"
#include "harmonia/scene/Mesh.hpp"
#include "harmonia/scene/Texture.hpp"

/// Abstract sink used by the scene loader to populate a renderer's Scene without
/// depending on its concrete GPU layout.
///
/// The interface is split into **meshes** (unique geometry, uploaded once, each
/// owning one BLAS) and **instances** (a mesh reference + transform + material):
///
///   addMesh / addSphereMesh  → register a unique mesh, return its index
///   addInstance              → place a mesh with a transform + material
///
/// This lets the loader import each unique mesh a single time and instance it
/// many times (true GPU instancing — N TLAS instances referencing one BLAS).
///
/// Each renderer's Scene implements this interface.  The implementations may
/// diverge where the renderer requires it — e.g. addSphereMesh is an analytic
/// sphere (AABB BLAS) in Hyperion (path tracer) but is tessellated into a
/// triangle mesh in Theia (rasterizer).  The loader stays renderer-agnostic.
class ISceneBuilder {
  public:
    virtual ~ISceneBuilder() = default;

    [[nodiscard]] virtual std::uint32_t addMaterial(Material&& mat) = 0;

    [[nodiscard]] virtual std::uint32_t addTexture(Texture&& texture) = 0;

    /// Register a unique triangle mesh (object space). Returns its mesh index,
    /// or uint32_max on failure.
    [[nodiscard]] virtual std::uint32_t
    addMesh(const DeviceContext& ctx, const CommandPool& pool, MeshData&& data, std::string_view name) = 0;

    /// Register a unique sphere mesh of @p radius (object space, centred at the
    /// origin). Returns its mesh index, or uint32_max on failure.
    [[nodiscard]] virtual std::uint32_t
    addSphereMesh(const DeviceContext& ctx, const CommandPool& pool, float radius, std::string_view name) = 0;

    /// Place an instance of mesh @p meshIndex with @p xform and @p materialIdx.
    /// Returns the instance index, or uint32_max on failure.
    [[nodiscard]] virtual std::uint32_t
    addInstance(std::uint32_t meshIndex, const Xform& xform, std::uint32_t materialIdx) = 0;
};
#endif // HARMONIA_SCENE_ISCENEBUILDER_HPP
