#ifndef HARMONIA_CORE_ONESHOT_HPP
#define HARMONIA_CORE_ONESHOT_HPP

#include <volk/volk.h>

#include <cstdint>
#include <span>

namespace harmonia {

[[nodiscard]] inline VkResult createFence(VkDevice device, VkFence* fence) noexcept {
    const VkFenceCreateInfo fenceInfo{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0U,
    };
    return vkCreateFence(device, &fenceInfo, nullptr, fence);
}

[[nodiscard]] inline VkResult submitOneShot(VkQueue queue,
                                            VkCommandBuffer cmd,
                                            VkFence fence = VK_NULL_HANDLE,
                                            std::span<const VkSemaphoreSubmitInfo> waitSemaphores = {},
                                            std::span<const VkSemaphoreSubmitInfo> signalSemaphores = {}) noexcept {
    const VkCommandBufferSubmitInfo commandBufferInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .pNext = nullptr,
        .commandBuffer = cmd,
        .deviceMask = 0U,
    };
    const VkSubmitInfo2 submitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .pNext = nullptr,
        .flags = 0U,
        .waitSemaphoreInfoCount = static_cast<std::uint32_t>(waitSemaphores.size()),
        .pWaitSemaphoreInfos = waitSemaphores.data(),
        .commandBufferInfoCount = 1U,
        .pCommandBufferInfos = &commandBufferInfo,
        .signalSemaphoreInfoCount = static_cast<std::uint32_t>(signalSemaphores.size()),
        .pSignalSemaphoreInfos = signalSemaphores.data(),
    };
    return vkQueueSubmit2(queue, 1U, &submitInfo, fence);
}

} // namespace harmonia

#endif // HARMONIA_CORE_ONESHOT_HPP
