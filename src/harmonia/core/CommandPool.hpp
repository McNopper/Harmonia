#ifndef HARMONIA_CORE_COMMANDPOOL_HPP
#define HARMONIA_CORE_COMMANDPOOL_HPP

#include <cstdint>
#include <expected>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/VulkanHandle.hpp"

namespace harmonia {

class CommandPool {
  public:
    [[nodiscard]] static std::expected<CommandPool, VkResult> create(const DeviceContext& ctx,
                                                                     std::uint32_t queueFamily);

    CommandPool() = default;
    CommandPool(const CommandPool&) = delete;
    CommandPool& operator=(const CommandPool&) = delete;
    CommandPool(CommandPool&&) noexcept = default;
    CommandPool& operator=(CommandPool&&) noexcept = default;
    ~CommandPool() noexcept = default;

    [[nodiscard]] std::expected<VkCommandBuffer, VkResult> allocate() const;
    void free(VkCommandBuffer cmd) const noexcept;

    [[nodiscard]] std::expected<VkCommandBuffer, VkResult> beginOneShot() const;
    VkResult endOneShot(VkCommandBuffer cmd) const noexcept;

    [[nodiscard]] VkCommandPool handle() const noexcept { return m_pool; }

  private:
    VkDevice m_device = VK_NULL_HANDLE;
    harmonia::UniqueCommandPool m_pool;
    VkQueue m_queue = VK_NULL_HANDLE;
};

} // namespace harmonia

#endif // HARMONIA_CORE_COMMANDPOOL_HPP
