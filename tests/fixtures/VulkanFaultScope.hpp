#ifndef TESTS_FIXTURES_VULKANFAULTSCOPE_HPP
#define TESTS_FIXTURES_VULKANFAULTSCOPE_HPP

// Test-only serial fault injection into volk's loaded entry points. Install only
// AFTER context/device loading; restore before another test can use Vulkan.
#include <volk/volk.h>

#include <cstdint>

class VulkanFaultScope {
  public:
    VulkanFaultScope() = default;
    VulkanFaultScope(const VulkanFaultScope&) = delete;
    VulkanFaultScope& operator=(const VulkanFaultScope&) = delete;
    ~VulkanFaultScope() {
        if (!m_installed)
            return;
        vkWaitSemaphores = m_wait;
        vkQueueSubmit2 = m_submit;
        vkDeviceWaitIdle = m_idle;
        vkDestroySemaphore = m_destroy;
        vkFreeCommandBuffers = m_free;
        vkCreateComputePipelines = m_compute;
        vkResetCommandBuffer = m_reset;
        vkDestroyImage = m_destroyImage;
        vkDestroyImageView = m_destroyView;
        vkDestroyPipeline = m_destroyPipeline;
        vkCreateSwapchainKHR = m_createSwapchain;
        s_active = nullptr;
    }
    void arm() noexcept {
        if (m_installed)
            return;
        m_wait = vkWaitSemaphores;
        m_submit = vkQueueSubmit2;
        m_idle = vkDeviceWaitIdle;
        m_destroy = vkDestroySemaphore;
        m_free = vkFreeCommandBuffers;
        m_compute = vkCreateComputePipelines;
        m_reset = vkResetCommandBuffer;
        m_destroyImage = vkDestroyImage;
        m_destroyView = vkDestroyImageView;
        m_destroyPipeline = vkDestroyPipeline;
        m_createSwapchain = vkCreateSwapchainKHR;
        s_active = this;
        vkWaitSemaphores = wait;
        vkQueueSubmit2 = submit;
        vkDeviceWaitIdle = idle;
        vkDestroySemaphore = destroy;
        vkFreeCommandBuffers = freeBuffers;
        vkCreateComputePipelines = compute;
        vkResetCommandBuffer = reset;
        vkDestroyImage = destroyImage;
        vkDestroyImageView = destroyView;
        vkDestroyPipeline = destroyPipeline;
        vkCreateSwapchainKHR = createSwapchain;
        m_installed = true;
    }
    static VkResult cleanupForTest(VkDevice device) noexcept {
        // Destruction safety net, deliberately outside the subject/oracle counters.
        return s_active != nullptr ? s_active->m_idle(device) : vkDeviceWaitIdle(device);
    }
    bool failWaitOnce = false;
    bool failSubmitOnce = false;
    bool failComputeOnce = false;
    std::uint32_t idleAllocationFailures = 0;
    bool waitInjected = false, submitInjected = false;
    bool computeInjected = false;
    bool retiredBySubject = false, unsafeSemaphoreDestruction = false, unsafeCommandFree = false;
    bool testRecoveryUsed = false;
    std::uint32_t idleCalls = 0;
    // Release-lifetime ordering observers (also used by generic capture tests).
    bool idleInjected = false, unsafeReset = false, unsafeResourceDestruction = false;
    bool failSwapchain = false, swapchainInjected = false;
    std::uint32_t swapchainCalls = 0;
    VkDevice deviceForReset = VK_NULL_HANDLE;

  private:
    static VKAPI_ATTR VkResult VKAPI_CALL compute(VkDevice device,
                                                  VkPipelineCache cache,
                                                  std::uint32_t count,
                                                  const VkComputePipelineCreateInfo* infos,
                                                  const VkAllocationCallbacks* allocator,
                                                  VkPipeline* pipelines) {
        auto& self = *s_active;
        if (self.failComputeOnce) {
            self.failComputeOnce = false;
            self.computeInjected = true;
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        return self.m_compute(device, cache, count, infos, allocator, pipelines);
    }
    static VKAPI_ATTR VkResult VKAPI_CALL wait(VkDevice device,
                                               const VkSemaphoreWaitInfo* info,
                                               std::uint64_t timeout) {
        auto& self = *s_active;
        if (self.failWaitOnce) {
            self.failWaitOnce = false;
            self.waitInjected = true;
            self.m_waitedSemaphore = info->pSemaphores[0];
            self.retiredBySubject = false;
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        return self.m_wait(device, info, timeout);
    }
    static VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue queue,
                                                 std::uint32_t count,
                                                 const VkSubmitInfo2* infos,
                                                 VkFence fence) {
        auto& self = *s_active;
        if (self.failSubmitOnce) {
            self.failSubmitOnce = false;
            self.submitInjected = true;
            self.retiredBySubject = false;
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        return self.m_submit(queue, count, infos, fence);
    }
    static VKAPI_ATTR VkResult VKAPI_CALL idle(VkDevice device) {
        auto& self = *s_active;
        ++self.idleCalls;
        if (self.idleAllocationFailures != 0) {
            --self.idleAllocationFailures;
            self.idleInjected = true;
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        const auto result = self.m_idle(device);
        if (result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST)
            self.retiredBySubject = true;
        return result;
    }
    static VKAPI_ATTR void VKAPI_CALL destroy(VkDevice device,
                                              VkSemaphore semaphore,
                                              const VkAllocationCallbacks* allocator) {
        auto& self = *s_active;
        if (self.waitInjected && semaphore == self.m_waitedSemaphore && !self.retiredBySubject) {
            self.unsafeSemaphoreDestruction = true;
            // Independent oracle, not the fix: make the OLD broken implementation
            // safe enough to test, but do not count this as subject retirement.
            self.testRecoveryUsed = true;
            self.m_idle(device);
        }
        self.protectDestruction(device);
        self.m_destroy(device, semaphore, allocator);
    }
    static VKAPI_ATTR void VKAPI_CALL freeBuffers(VkDevice device,
                                                  VkCommandPool pool,
                                                  std::uint32_t count,
                                                  const VkCommandBuffer* buffers) {
        auto& self = *s_active;
        if (self.waitInjected && !self.retiredBySubject) {
            self.unsafeCommandFree = true;
            self.testRecoveryUsed = true;
            self.m_idle(device);
        }
        self.protectDestruction(device);
        self.m_free(device, pool, count, buffers);
    }
    [[nodiscard]] bool requiresRetirement() const {
        return (waitInjected || submitInjected || idleInjected) && !retiredBySubject;
    }
    void protectDestruction(VkDevice device) {
        if (requiresRetirement()) {
            unsafeResourceDestruction = true;
            // Independent safety net: deliberately not credited as subject retirement.
            static_cast<void>(m_idle(device));
        }
    }
    static VKAPI_ATTR VkResult VKAPI_CALL reset(VkCommandBuffer cmd, VkCommandBufferResetFlags flags) {
        auto& self = *s_active;
        if (self.requiresRetirement()) {
            self.unsafeReset = true;
            if (self.deviceForReset != VK_NULL_HANDLE)
                static_cast<void>(self.m_idle(self.deviceForReset));
        }
        return self.m_reset(cmd, flags);
    }
    static VKAPI_ATTR void VKAPI_CALL destroyImage(VkDevice device,
                                                   VkImage image,
                                                   const VkAllocationCallbacks* allocator) {
        s_active->protectDestruction(device);
        s_active->m_destroyImage(device, image, allocator);
    }
    static VKAPI_ATTR void VKAPI_CALL destroyView(VkDevice device,
                                                  VkImageView view,
                                                  const VkAllocationCallbacks* allocator) {
        s_active->protectDestruction(device);
        s_active->m_destroyView(device, view, allocator);
    }
    static VKAPI_ATTR void VKAPI_CALL destroyPipeline(VkDevice device,
                                                      VkPipeline pipeline,
                                                      const VkAllocationCallbacks* allocator) {
        s_active->protectDestruction(device);
        s_active->m_destroyPipeline(device, pipeline, allocator);
    }
    static VKAPI_ATTR VkResult VKAPI_CALL createSwapchain(VkDevice device,
                                                          const VkSwapchainCreateInfoKHR* info,
                                                          const VkAllocationCallbacks* allocator,
                                                          VkSwapchainKHR* swapchain) {
        auto& self = *s_active;
        ++self.swapchainCalls;
        if (self.failSwapchain) {
            self.failSwapchain = false;
            self.swapchainInjected = true;
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        return self.m_createSwapchain(device, info, allocator, swapchain);
    }
    inline static VulkanFaultScope* s_active{};
    bool m_installed = false;
    VkSemaphore m_waitedSemaphore{};
    PFN_vkWaitSemaphores m_wait{};
    PFN_vkQueueSubmit2 m_submit{};
    PFN_vkDeviceWaitIdle m_idle{};
    PFN_vkDestroySemaphore m_destroy{};
    PFN_vkFreeCommandBuffers m_free{};
    PFN_vkCreateComputePipelines m_compute{};
    PFN_vkResetCommandBuffer m_reset{};
    PFN_vkDestroyImage m_destroyImage{};
    PFN_vkDestroyImageView m_destroyView{};
    PFN_vkDestroyPipeline m_destroyPipeline{};
    PFN_vkCreateSwapchainKHR m_createSwapchain{};
};

#endif // TESTS_FIXTURES_VULKANFAULTSCOPE_HPP
