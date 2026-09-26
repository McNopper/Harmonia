#include "harmonia/core/CommandPool.hpp"

#include <cstdint>

#include "harmonia/core/OneShot.hpp"

namespace harmonia {

namespace {} // namespace

std::expected<CommandPool, VkResult> CommandPool::create(const DeviceContext& ctx, std::uint32_t queueFamily) {
    if (!ctx.isValid() || ctx.graphicsQueue == VK_NULL_HANDLE) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    const VkCommandPoolCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = queueFamily,
    };

    CommandPool pool;
    VkCommandPool commandPool{};
    const VkResult result = vkCreateCommandPool(ctx.device, &createInfo, nullptr, &commandPool);
    if (result != VK_SUCCESS) {
        return std::unexpected(result);
    }
    pool.m_pool = harmonia::UniqueCommandPool{ctx.device, commandPool};

    pool.m_device = ctx.device;
    pool.m_queue = ctx.graphicsQueue;
    ctx.setDebugName(VK_OBJECT_TYPE_COMMAND_POOL, pool.m_pool.get(), "harmonia Command Pool");
    return pool;
}

std::expected<VkCommandBuffer, VkResult> CommandPool::allocate() const {
    if (m_device == VK_NULL_HANDLE || m_pool == VK_NULL_HANDLE) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    const VkCommandBufferAllocateInfo allocateInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr,
        .commandPool = m_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1U,
    };

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    const VkResult result = vkAllocateCommandBuffers(m_device, &allocateInfo, &cmd);
    if (result != VK_SUCCESS) {
        return std::unexpected(result);
    }

    return cmd;
}

void CommandPool::free(VkCommandBuffer cmd) const noexcept {
    if (m_device == VK_NULL_HANDLE || m_pool == VK_NULL_HANDLE || cmd == VK_NULL_HANDLE) {
        return;
    }

    vkFreeCommandBuffers(m_device, m_pool, 1U, &cmd);
}

std::expected<VkCommandBuffer, VkResult> CommandPool::beginOneShot() const {
    auto cmdResult = allocate();
    if (!cmdResult.has_value()) {
        return std::unexpected(cmdResult.error());
    }

    const VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };

    const VkResult beginResult = vkBeginCommandBuffer(*cmdResult, &beginInfo);
    if (beginResult != VK_SUCCESS) {
        free(*cmdResult);
        return std::unexpected(beginResult);
    }

    return *cmdResult;
}

VkResult CommandPool::endOneShot(VkCommandBuffer cmd) const noexcept {
    if (m_device == VK_NULL_HANDLE || m_pool == VK_NULL_HANDLE || m_queue == VK_NULL_HANDLE || cmd == VK_NULL_HANDLE) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const VkResult endResult = vkEndCommandBuffer(cmd);
    if (endResult != VK_SUCCESS) {
        free(cmd);
        return endResult;
    }

    // MOD2: timeline semaphore replaces VkFence for one-shot completion tracking.
    VkSemaphore semaphore = VK_NULL_HANDLE;
    VkResult result = harmonia::createTimelineSemaphore(m_device, &semaphore);
    if (result != VK_SUCCESS) {
        free(cmd);
        return result;
    }

    result = harmonia::submitOneShot(m_queue, cmd, semaphore, 1);
    if (result == VK_SUCCESS) {
        result = harmonia::waitTimelineSemaphore(m_device, semaphore, 1);
    }

    vkDestroySemaphore(m_device, semaphore, nullptr);
    free(cmd);
    return result;
}

} // namespace harmonia
