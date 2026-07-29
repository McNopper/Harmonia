#ifndef HARMONIA_RENDERER_ACCELERATIONSTRUCTURE_HPP
#define HARMONIA_RENDERER_ACCELERATIONSTRUCTURE_HPP

#include <volk/volk.h>

#include <expected>
#include <string_view>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/Buffer.hpp"

class AccelerationStructure {
  public:
    AccelerationStructure() = default;
    ~AccelerationStructure() noexcept;

    AccelerationStructure(AccelerationStructure&& other) noexcept;
    AccelerationStructure& operator=(AccelerationStructure&& other) noexcept;

    AccelerationStructure(const AccelerationStructure&) = delete;
    AccelerationStructure& operator=(const AccelerationStructure&) = delete;

    [[nodiscard]] static std::expected<AccelerationStructure, VkResult> create(const DeviceContext& ctx,
                                                                               VkAccelerationStructureTypeKHR type,
                                                                               VkDeviceSize size,
                                                                               std::string_view debugName = "");

    [[nodiscard]] VkAccelerationStructureKHR handle() const noexcept { return m_handle; }
    [[nodiscard]] VkDeviceAddress deviceAddress() const noexcept { return m_deviceAddress; }
    [[nodiscard]] const Buffer& buffer() const noexcept { return m_buffer; }

  private:
    void reset() noexcept;

    VkDevice m_device{VK_NULL_HANDLE};
    Buffer m_buffer{};
    VkAccelerationStructureKHR m_handle{VK_NULL_HANDLE};
    VkDeviceAddress m_deviceAddress{};
};

/// Scratch buffer for an acceleration-structure build, sized from @p sizes and
/// aligned to the device's `minAccelerationStructureScratchOffsetAlignment`.
/// `alignedAddress` is the scratch base address rounded up to that alignment —
/// assign directly to `VkAccelerationStructureBuildGeometryInfoKHR::scratchData.deviceAddress`.
struct AccelerationStructureScratch {
    Buffer buffer;
    VkDeviceAddress alignedAddress = 0;
};

[[nodiscard]] std::expected<AccelerationStructureScratch, VkResult>
createAccelerationStructureScratch(const DeviceContext& ctx,
                                   const VkAccelerationStructureBuildSizesInfoKHR& sizes,
                                   std::string_view debugName);
#endif // HARMONIA_RENDERER_ACCELERATIONSTRUCTURE_HPP
