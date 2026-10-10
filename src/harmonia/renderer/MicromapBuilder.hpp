#ifndef HARMONIA_RENDERER_MICROMAPBUILDER_HPP
#define HARMONIA_RENDERER_MICROMAPBUILDER_HPP

#include <volk/volk.h>

#include <cstdint>
#include <expected>
#include <string_view>

#include "aether/types/OpacityMicromap.hpp"
#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/Buffer.hpp"
#include "harmonia/core/CommandPool.hpp"
#include "harmonia/renderer/AccelerationStructure.hpp"

namespace harmonia {

/// Move-only owner of a built opacity micromap and all the device buffers it
/// needs: the packed-state `data`, the per-record `triangle` array, and the
/// per-base-triangle `index` buffer that links BLAS triangles to micromap
/// records. VK_KHR_opacity_micromap folds micromaps into the acceleration-
/// structure API (the extension proposal's issue 6): the micromap IS an
/// `AccelerationStructure` with `type = VK_ACCELERATION_STRUCTURE_TYPE_
/// OPACITY_MICROMAP_KHR`, built through `vkCmdBuildAccelerationStructuresKHR`.
/// A `Micromap` must outlive any BLAS that references it — `TriangleMesh` owns
/// both, so they share a lifetime.
class Micromap {
  public:
    Micromap() = default;
    ~Micromap() noexcept = default;
    Micromap(Micromap&&) noexcept = default;
    Micromap& operator=(Micromap&&) noexcept = default;

    Micromap(const Micromap&) = delete;
    Micromap& operator=(const Micromap&) = delete;

    /// The built micromap handle — chain into
    /// `VkAccelerationStructureTrianglesOpacityMicromapKHR::micromap`.
    [[nodiscard]] VkAccelerationStructureKHR handle() const noexcept { return m_as.handle(); }
    /// Per-base-triangle index buffer device address
    /// (`VkAccelerationStructureTrianglesOpacityMicromapKHR::indexBuffer`).
    [[nodiscard]] VkDeviceAddress indexBufferAddress() const noexcept { return m_indexBuffer.deviceAddress(); }
    /// One index per base triangle (the BLAS primitive count).
    [[nodiscard]] std::uint32_t indexCount() const noexcept { return m_indexCount; }

  private:
    Buffer m_dataBuffer{};
    Buffer m_triangleBuffer{};
    Buffer m_indexBuffer{};
    AccelerationStructure m_as{};
    AccelerationStructureScratch m_scratch{};
    std::uint32_t m_indexCount = 0;
    friend class MicromapBuilder;
};

/// Builds an opacity micromap from a parsed Aether group — device-side-only, via
/// the VK_KHR_opacity_micromap acceleration-structure path
/// (`vkCmdBuildAccelerationStructuresKHR` with `VK_GEOMETRY_TYPE_MICROMAP_KHR`;
/// no host builds). The capability is probed via
/// `DeviceContext::opacityMicromapSupported`; callers must gate on that.
class MicromapBuilder {
  public:
    /// Build the micromap for @p group. Uploads the packed-state data, the
    /// `VkMicromapTriangleKHR` records and the per-triangle index buffer, sizes
    /// the storage/scratch from `vkGetAccelerationStructureBuildSizesKHR`, then
    /// records the build in a one-shot command buffer.
    [[nodiscard]] static std::expected<Micromap, VkResult> build(const DeviceContext& ctx,
                                                                 const CommandPool& pool,
                                                                 const aether::OpacityMicromapGroup& group,
                                                                 std::string_view debugName = "");
};

} // namespace harmonia

#endif // HARMONIA_RENDERER_MICROMAPBUILDER_HPP
