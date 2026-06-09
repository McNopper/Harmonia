#pragma once

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include <cstdint>
#include <string_view>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/CommandPool.hpp"
#include "harmonia/scene/Material.hpp"
#include "harmonia/scene/Mesh.hpp"
#include "harmonia/scene/Texture.hpp"

/// Abstract sink used by scene importers/loaders to populate a renderer's
/// Scene without depending on its concrete GPU layout.
///
/// Each renderer's Scene implements this interface.  The implementations may
/// diverge where the renderer requires it — e.g. addSphere is an analytic
/// sphere in Hyperion (path tracer) but is tessellated into a mesh in Theia
/// (rasterizer).  Importers and the SceneLoader stay renderer-agnostic by
/// talking only to this interface.
class ISceneBuilder {
  public:
    virtual ~ISceneBuilder() = default;

    [[nodiscard]] virtual uint32_t addMaterial(Material mat) = 0;

    [[nodiscard]] virtual uint32_t addTexture(Texture texture) = 0;

    [[nodiscard]] virtual uint32_t addMesh(const DeviceContext& ctx,
                                           const CommandPool& pool,
                                           MeshData data,
                                           uint32_t materialIdx,
                                           std::string_view name) = 0;

    [[nodiscard]] virtual uint32_t addSphere(const DeviceContext& ctx,
                                             const CommandPool& pool,
                                             glm::vec3 center,
                                             float radius,
                                             uint32_t materialIdx) = 0;
};
