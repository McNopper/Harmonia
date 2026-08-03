#ifndef HARMONIA_CORE_BUFFER_HPP
#define HARMONIA_CORE_BUFFER_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

#include "harmonia/DeviceContext.hpp"

namespace harmonia {

class CommandPool;

[[nodiscard]] constexpr VkDeviceSize bufferAlignUp(VkDeviceSize value, VkDeviceSize alignment) noexcept {
    return alignment == 0 ? value : ((value + alignment - 1) / alignment) * alignment;
}

class Buffer {
  public:
    [[nodiscard]] static std::expected<Buffer, VkResult> create(const DeviceContext& ctx,
                                                                VkDeviceSize size,
                                                                VkBufferUsageFlags usage,
                                                                VmaMemoryUsage memUsage,
                                                                std::string_view debugName = "",
                                                                VkDeviceSize minAlignment = 0);

    // Stage-upload helper: allocates a host staging buffer, copies bytes into it,
    // then records a device-side copy to a DEVICE_LOCAL buffer and waits for it.
    // Enforces a 16-byte minimum size for safe empty-span sentinel uploads.
    // @p minAlignment requests a minimum allocation alignment (power of two) — used
    // for buffers whose device address must be aligned (e.g. micromap data/triangle
    // arrays require 256-byte alignment per VUID-vkCmdBuildMicromapsEXT-pInfos-07515).
    [[nodiscard]] static std::expected<Buffer, VkResult> upload(const DeviceContext& ctx,
                                                                const CommandPool& pool,
                                                                std::span<const std::byte> bytes,
                                                                VkBufferUsageFlags usage,
                                                                std::string_view name = "",
                                                                VkDeviceSize minAlignment = 0);

    Buffer() = default;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;
    ~Buffer();

    [[nodiscard]] VkBuffer handle() const noexcept { return m_buffer; }
    [[nodiscard]] VkDeviceSize size() const noexcept { return m_size; }
    [[nodiscard]] void* mappedData() const noexcept { return m_mapped; }
    [[nodiscard]] VkDeviceAddress deviceAddress() const noexcept;
    [[nodiscard]] bool isValid() const noexcept { return m_buffer != VK_NULL_HANDLE; }

    void uploadData(const void* data, VkDeviceSize size, VkDeviceSize offset = 0);

  private:
    void destroy() noexcept;

    VkBuffer m_buffer = VK_NULL_HANDLE;
    VmaAllocation m_allocation = VK_NULL_HANDLE;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    VkDeviceSize m_size = 0;
    std::uint32_t m_queueFamily = 0;
    void* m_mapped = nullptr;
};

} // namespace harmonia

#endif // HARMONIA_CORE_BUFFER_HPP
