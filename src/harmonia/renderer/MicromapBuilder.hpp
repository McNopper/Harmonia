#ifndef HARMONIA_RENDERER_MICROMAPBUILDER_HPP
#define HARMONIA_RENDERER_MICROMAPBUILDER_HPP

#include <volk/volk.h>

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

#include "aether/types/OpacityMicromap.hpp"
#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/Buffer.hpp"
#include "harmonia/core/CommandPool.hpp"
#include "harmonia/core/VulkanHandle.hpp"

namespace harmonia {

/// Move-only owner of a built opacity micromap (`VkMicromapEXT`) and all the
/// device buffers it needs: the packed-state `data`, the per-record `triangle`
/// array, the build `storage` + `scratch`, and the per-base-triangle
/// `index` buffer that links BLAS triangles to micromap records. A `Micromap`
/// must outlive any BLAS that references it — `TriangleMesh` owns both, so they
/// share a lifetime.
class Micromap {
  public:
    Micromap() = default;
    ~Micromap() noexcept = default;
    Micromap(Micromap&&) noexcept = default;
    Micromap& operator=(Micromap&&) noexcept = default;

    Micromap(const Micromap&) = delete;
    Micromap& operator=(const Micromap&) = delete;

    /// The built micromap handle — chain into
    /// `VkAccelerationStructureTrianglesOpacityMicromapEXT::micromap`.
    [[nodiscard]] VkMicromapEXT handle() const noexcept { return m_handle.get(); }
    /// Per-base-triangle index buffer device address
    /// (`VkAccelerationStructureTrianglesOpacityMicromapEXT::indexBuffer`).
    [[nodiscard]] VkDeviceAddress indexBufferAddress() const noexcept { return m_indexBuffer.deviceAddress(); }
    /// One index per base triangle (the BLAS primitive count).
    [[nodiscard]] std::uint32_t indexCount() const noexcept { return m_indexCount; }
    /// The converted `VkMicromapUsageEXT` histogram — feed into both
    /// `VkMicromapBuildInfoEXT::pUsageCounts` and the BLAS-chain struct
    /// `VkAccelerationStructureTrianglesOpacityMicromapEXT::pUsageCounts`.
    [[nodiscard]] std::span<const VkMicromapUsageEXT> usage() const noexcept { return m_usage; }

  private:
    Buffer m_dataBuffer{};
    Buffer m_triangleBuffer{};
    Buffer m_storageBuffer{};
    Buffer m_scratchBuffer{};
    Buffer m_indexBuffer{};
    std::vector<VkMicromapUsageEXT> m_usage;
    UniqueMicromapEXT m_handle;
    std::uint32_t m_indexCount = 0;
    friend class MicromapBuilder;
};

/// Builds a `VkMicromapEXT` (opacity micromap) from a parsed Aether group,
/// mirroring the device-side-only `vkCmdBuildAccelerationStructuresKHR` rule
/// (`vkCmdBuildMicromapsEXT`, no host builds). The capability is probed via
/// `DeviceContext::opacityMicromapSupported`; callers must gate on that.
class MicromapBuilder {
  public:
    /// Build the micromap for @p group. Uploads the packed-state data, the
    /// `VkMicromapTriangleEXT` records and the per-triangle index buffer, sizes
    /// the storage/scratch from `vkGetMicromapBuildSizesEXT`, then records the
    /// build in a one-shot command buffer.
    [[nodiscard]] static std::expected<Micromap, VkResult> build(const DeviceContext& ctx,
                                                                 const CommandPool& pool,
                                                                 const aether::OpacityMicromapGroup& group,
                                                                 std::string_view debugName = "");
};

} // namespace harmonia

#endif // HARMONIA_RENDERER_MICROMAPBUILDER_HPP
