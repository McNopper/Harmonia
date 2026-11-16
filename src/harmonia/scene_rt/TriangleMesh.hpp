#ifndef HARMONIA_SCENE_RT_TRIANGLEMESH_HPP
#define HARMONIA_SCENE_RT_TRIANGLEMESH_HPP

#include <volk/volk.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/Buffer.hpp"
#include "harmonia/core/CommandPool.hpp"
#include "harmonia/rt/AccelerationStructure.hpp"
#include "harmonia/rt/MicromapBuilder.hpp"
#include "harmonia/scene/Geometry.hpp"
#include "harmonia/scene/Mesh.hpp"

namespace harmonia::scene_rt {

/// A unique triangle mesh with its ray-tracing state: the BLAS build, the
/// opacity-micromap participation and the TLAS instance emission. The pure
/// geometry data lives in harmonia::scene (MeshData / Mesh / MeshOpacity).
class TriangleMesh final : public harmonia::scene::Geometry {
  public:
    [[nodiscard]] static std::expected<std::unique_ptr<TriangleMesh>, VkResult>
    create(const DeviceContext& ctx,
           const core::CommandPool& pool,
           harmonia::scene::MeshData&& data,
           const harmonia::scene::MeshOpacity& opacity = {},
           std::string_view debugName = "");

    VkResult buildBlas(const DeviceContext& ctx, const core::CommandPool& pool) override;
    /// ANI4: in-place BLAS refit after the deformation pass rewrote the vertex
    /// buffer (requires `setDynamic(true)` before `buildBlas` - ALLOW_UPDATE).
    /// Success no-op for static meshes; handle/device address stay valid.
    [[nodiscard]] VkResult refitBlas(const DeviceContext& ctx, const core::CommandPool& pool) override;
    [[nodiscard]] VkAccelerationStructureInstanceKHR
    makeInstance(std::uint32_t instanceCustomIndex,
                 const harmonia::scene::Transform& transform) const noexcept override;
    [[nodiscard]] harmonia::scene::Aabb objectAabb() const noexcept override;

    [[nodiscard]] const core::Buffer& vertexBuffer() const noexcept;
    [[nodiscard]] const core::Buffer& indexBuffer() const noexcept;
    [[nodiscard]] std::uint32_t vertexCount() const noexcept;
    [[nodiscard]] std::uint32_t indexCount() const noexcept;
    [[nodiscard]] const harmonia::scene::MeshData& data() const noexcept;

  private:
    /// Shared geometry description for buildBlas and the ANI4 refitBlas (same
    /// vertex/index addresses - the deformation pass rewrites vertices in place).
    void makeBlasGeometry(VkAccelerationStructureGeometryKHR& geometry,
                          VkAccelerationStructureBuildRangeInfoKHR& rangeInfo) const;

    harmonia::scene::MeshData m_data{};
    harmonia::scene::Mesh m_mesh{};
    rt::AccelerationStructure m_accelerationStructure{};
    harmonia::scene::MeshOpacity m_opacity{}; ///< cutout participation; micromap baked in buildBlas.
    std::optional<rt::Micromap> m_micromap;   ///< built OMM, owns its device buffers.
    /// ANI4: build-sized refit scratch, allocated once on the first refitBlas.
    std::optional<rt::AccelerationStructureScratch> m_refitScratch;
    std::string m_debugName{};
};

} // namespace harmonia::scene_rt

#endif // HARMONIA_SCENE_RT_TRIANGLEMESH_HPP
