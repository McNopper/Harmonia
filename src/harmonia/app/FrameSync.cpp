#include "harmonia/app/FrameSync.hpp"

#include <volk/volk.h>

#include <cstdint>

#include "harmonia/core/Logger.hpp"

namespace harmonia {

namespace {

[[nodiscard]] VkResult createBinarySemaphore(VkDevice device, VkSemaphore& semaphore) {
    const VkSemaphoreCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
    };
    return vkCreateSemaphore(device, &info, nullptr, &semaphore);
}

[[nodiscard]] VkResult createTimelineSemaphore(VkDevice device, VkSemaphore& semaphore) {
    const VkSemaphoreTypeCreateInfo typeInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext = nullptr,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    const VkSemaphoreCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &typeInfo,
        .flags = 0,
    };
    return vkCreateSemaphore(device, &info, nullptr, &semaphore);
}

} // namespace

bool FrameSync::create(const DeviceContext& ctx, CommandPool& cmdPool, std::uint32_t swapchainImageCount) {
    m_device = ctx.device;

    for (FrameResources& frame : m_frames) {
        auto renderCmd = cmdPool.allocate();
        auto displayCmd = cmdPool.allocate();
        if (!renderCmd || !displayCmd) {
            Logger::error("Command buffer allocation failed");
            return false;
        }
        frame.renderCmd = *renderCmd;
        frame.displayCmd = *displayCmd;

        VkSemaphore imageSem{};
        if (createBinarySemaphore(m_device, imageSem) != VK_SUCCESS) {
            Logger::error("Semaphore creation failed");
            return false;
        }
        frame.imageAvailable = harmonia::UniqueSemaphore{m_device, imageSem};
    }

    m_renderComplete.reserve(swapchainImageCount);
    for (std::uint32_t i = 0; i < swapchainImageCount; ++i) {
        VkSemaphore sem{};
        if (createBinarySemaphore(m_device, sem) != VK_SUCCESS) {
            Logger::error("renderComplete semaphore creation failed");
            return false;
        }
        m_renderComplete.emplace_back(m_device, sem);
    }

    VkSemaphore timelineSem{};
    if (createTimelineSemaphore(m_device, timelineSem) != VK_SUCCESS) {
        Logger::error("Timeline semaphore creation failed");
        return false;
    }
    m_timelineSemaphore = harmonia::UniqueSemaphore{m_device, timelineSem};

    return true;
}

void FrameSync::destroy() noexcept {
    for (FrameResources& frame : m_frames) {
        frame = {};
    }
    m_renderComplete.clear();
    m_timelineSemaphore.reset();
    m_device = VK_NULL_HANDLE;
}

void FrameSync::onResize(std::uint32_t swapchainImageCount) noexcept {
    resetSlots();
    m_renderComplete.clear();
    m_renderComplete.reserve(swapchainImageCount);
    for (std::uint32_t i = 0; i < swapchainImageCount; ++i) {
        VkSemaphore sem{};
        if (createBinarySemaphore(m_device, sem) != VK_SUCCESS) {
            Logger::error("renderComplete semaphore recreate failed");
            return;
        }
        m_renderComplete.emplace_back(m_device, sem);
    }
}

void FrameSync::waitSlotComplete(std::uint32_t slot, std::uint64_t timeoutNs) const noexcept {
    const std::uint64_t value = m_frames[slot].completionValue;
    if (value == 0U) {
        return;
    }
    const VkSemaphore wait = m_timelineSemaphore;
    const VkSemaphoreWaitInfo waitInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .pNext = nullptr,
        .flags = 0,
        .semaphoreCount = 1,
        .pSemaphores = &wait,
        .pValues = &value,
    };
    vkWaitSemaphores(m_device, &waitInfo, timeoutNs);
}

void FrameSync::resetSlots() noexcept {
    for (FrameResources& frame : m_frames) {
        if (frame.renderCmd != VK_NULL_HANDLE) {
            vkResetCommandBuffer(frame.renderCmd, 0);
        }
        if (frame.displayCmd != VK_NULL_HANDLE) {
            vkResetCommandBuffer(frame.displayCmd, 0);
        }
        frame.completionValue = 0U;
    }
}

} // namespace harmonia
