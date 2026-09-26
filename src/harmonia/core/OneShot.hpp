#ifndef HARMONIA_CORE_ONESHOT_HPP
#define HARMONIA_CORE_ONESHOT_HPP

#include <volk/volk.h>

#include <cstdint>
#include <span>

namespace harmonia {

/// MOD2: timeline semaphore for one-shot submits (replaces VkFence — unifies all sync
/// on one primitive; the frame path already uses timeline semaphores exclusively).
[[nodiscard]] inline VkResult createTimelineSemaphore(VkDevice device, VkSemaphore* semaphore) noexcept {
    const VkSemaphoreTypeCreateInfo typeInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext = nullptr,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    const VkSemaphoreCreateInfo semaphoreInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &typeInfo,
        .flags = 0U,
    };
    return vkCreateSemaphore(device, &semaphoreInfo, nullptr, semaphore);
}

/// Host-side wait for a timeline semaphore to reach @p value (replaces vkWaitForFences).
[[nodiscard]] inline VkResult
waitTimelineSemaphore(VkDevice device, VkSemaphore semaphore, std::uint64_t value) noexcept {
    const VkSemaphoreWaitInfo waitInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .pNext = nullptr,
        .flags = 0U,
        .semaphoreCount = 1U,
        .pSemaphores = &semaphore,
        .pValues = &value,
    };
    return vkWaitSemaphores(device, &waitInfo, ~0ULL);
}

/// One-shot submit: @p cmd is submitted to @p queue; @p timelineSemaphore is signalled
/// at @p signalValue on completion (replaces the VkFence parameter — MOD2).
[[nodiscard]] inline VkResult submitOneShot(VkQueue queue,
                                            VkCommandBuffer cmd,
                                            VkSemaphore timelineSemaphore,
                                            std::uint64_t signalValue,
                                            std::span<const VkSemaphoreSubmitInfo> waitSemaphores = {},
                                            std::span<const VkSemaphoreSubmitInfo> signalSemaphores = {}) noexcept {
    const VkCommandBufferSubmitInfo commandBufferInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .pNext = nullptr,
        .commandBuffer = cmd,
        .deviceMask = 0U,
    };
    // MOD2: the timeline signal replaces the VkFence.
    VkSemaphoreSubmitInfo signalInfos[9]; // 1 timeline + up to 8 caller signals
    std::uint32_t signalCount = 0;
    if (timelineSemaphore != VK_NULL_HANDLE) {
        signalInfos[signalCount++] = VkSemaphoreSubmitInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = timelineSemaphore,
            .value = signalValue,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .deviceIndex = 0,
        };
    }
    for (const auto& sig : signalSemaphores) {
        signalInfos[signalCount++] = sig;
    }
    const VkSubmitInfo2 submitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .pNext = nullptr,
        .flags = 0U,
        .waitSemaphoreInfoCount = static_cast<std::uint32_t>(waitSemaphores.size()),
        .pWaitSemaphoreInfos = waitSemaphores.data(),
        .commandBufferInfoCount = 1U,
        .pCommandBufferInfos = &commandBufferInfo,
        .signalSemaphoreInfoCount = signalCount,
        .pSignalSemaphoreInfos = signalInfos,
    };
    return vkQueueSubmit2(queue, 1U, &submitInfo, VK_NULL_HANDLE);
}

} // namespace harmonia

#endif // HARMONIA_CORE_ONESHOT_HPP
