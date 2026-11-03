#ifndef TESTS_FIXTURES_VULKANFAULTSCOPE_HPP
#define TESTS_FIXTURES_VULKANFAULTSCOPE_HPP

#include <volk/volk.h>

#include <cstdint>

// Serial test-only faults. The safety-net wait is deliberately not credited as
// retirement by the implementation: old unsafe code fails without causing a GPU UAF.
class VulkanFaultScope {
  public:
    VulkanFaultScope() = default;
    VulkanFaultScope(const VulkanFaultScope&) = delete;
    VulkanFaultScope& operator=(const VulkanFaultScope&) = delete;
    ~VulkanFaultScope() {
        if (!m_armed)
            return;
        vkWaitSemaphores = m_wait;
        vkDeviceWaitIdle = m_idle;
        vkDestroySemaphore = m_destroy;
        vkFreeCommandBuffers = m_free;
        vkQueueSubmit2 = m_submit;
        vkCreateComputePipelines = m_compute;
        vkResetCommandBuffer = m_reset;
        vkDestroyImage = m_destroyImage;
        vkDestroyImageView = m_destroyView;
        vkDestroyPipeline = m_destroyPipeline;
        vkCreateSwapchainKHR = m_createSwapchain;
        s_active = nullptr;
    }
    void arm() {
        if (m_armed)
            return;
        m_wait = vkWaitSemaphores;
        m_idle = vkDeviceWaitIdle;
        m_destroy = vkDestroySemaphore;
        m_free = vkFreeCommandBuffers;
        m_submit = vkQueueSubmit2;
        m_compute = vkCreateComputePipelines;
        m_reset = vkResetCommandBuffer;
        m_destroyImage = vkDestroyImage;
        m_destroyView = vkDestroyImageView;
        m_destroyPipeline = vkDestroyPipeline;
        m_createSwapchain = vkCreateSwapchainKHR;
        s_active = this;
        vkWaitSemaphores = wait;
        vkDeviceWaitIdle = idle;
        vkDestroySemaphore = destroy;
        vkFreeCommandBuffers = freeBuffers;
        vkQueueSubmit2 = submit;
        vkCreateComputePipelines = compute;
        vkResetCommandBuffer = reset;
        vkDestroyImage = destroyImage;
        vkDestroyImageView = destroyView;
        vkDestroyPipeline = destroyPipeline;
        vkCreateSwapchainKHR = createSwapchain;
        m_armed = true;
    }
    static void cleanup(VkDevice device) {
        if (s_active)
            s_active->m_idle(device);
        else
            vkDeviceWaitIdle(device);
    }
    bool failWait = false, failSubmit = false, failCompute = false;
    bool waitInjected = false, submitInjected = false, computeInjected = false;
    bool retired = false, unsafeDestroy = false, unsafeFree = false;
    bool idleInjected = false, unsafeReset = false, unsafeResourceDestruction = false;
    bool failSwapchain = false, swapchainInjected = false;
    std::uint32_t swapchainCalls = 0;
    std::uint32_t idleFailures = 0, idleCalls = 0;

  private:
    static VKAPI_ATTR VkResult VKAPI_CALL wait(VkDevice device,
                                               const VkSemaphoreWaitInfo* info,
                                               std::uint64_t timeout) {
        auto& self = *s_active;
        if (self.failWait) {
            self.failWait = false;
            self.waitInjected = true;
            self.m_waited = info->pSemaphores[0];
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        return self.m_wait(device, info, timeout);
    }
    static VKAPI_ATTR VkResult VKAPI_CALL idle(VkDevice device) {
        auto& self = *s_active;
        ++self.idleCalls;
        if (self.idleFailures) {
            --self.idleFailures;
            self.idleInjected = true;
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        const auto result = self.m_idle(device);
        if (result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST)
            self.retired = true;
        return result;
    }
    static VKAPI_ATTR void VKAPI_CALL destroy(VkDevice device,
                                              VkSemaphore semaphore,
                                              const VkAllocationCallbacks* allocator) {
        auto& self = *s_active;
        if (self.waitInjected && semaphore == self.m_waited && !self.retired) {
            self.unsafeDestroy = true;
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
        if (self.waitInjected && !self.retired) {
            self.unsafeFree = true;
            self.m_idle(device);
        }
        self.protectDestruction(device);
        self.m_free(device, pool, count, buffers);
    }
    static VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue queue,
                                                 std::uint32_t count,
                                                 const VkSubmitInfo2* infos,
                                                 VkFence fence) {
        auto& self = *s_active;
        if (self.failSubmit) {
            self.failSubmit = false;
            self.submitInjected = true;
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        return self.m_submit(queue, count, infos, fence);
    }
    static VKAPI_ATTR VkResult VKAPI_CALL compute(VkDevice device,
                                                  VkPipelineCache cache,
                                                  std::uint32_t count,
                                                  const VkComputePipelineCreateInfo* infos,
                                                  const VkAllocationCallbacks* allocator,
                                                  VkPipeline* pipelines) {
        auto& self = *s_active;
        if (self.failCompute) {
            self.failCompute = false;
            self.computeInjected = true;
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        return self.m_compute(device, cache, count, infos, allocator, pipelines);
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
    bool requiresRetirement() const { return (waitInjected || submitInjected || idleInjected) && !retired; }
    void protectDestruction(VkDevice device) {
        if (requiresRetirement()) {
            unsafeResourceDestruction = true;
            m_idle(device); // independent safety net, never credited as subject retirement
        }
    }
    static VKAPI_ATTR VkResult VKAPI_CALL reset(VkCommandBuffer cmd, VkCommandBufferResetFlags flags) {
        auto& self = *s_active;
        if (self.requiresRetirement()) {
            self.unsafeReset = true;
            // The test owns a device because a command buffer does not expose it.
            if (self.deviceForReset != VK_NULL_HANDLE)
                self.m_idle(self.deviceForReset);
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

  public:
    VkDevice deviceForReset = VK_NULL_HANDLE;

  private:
    inline static VulkanFaultScope* s_active = nullptr;
    bool m_armed = false;
    VkSemaphore m_waited = VK_NULL_HANDLE;
    PFN_vkWaitSemaphores m_wait{};
    PFN_vkDeviceWaitIdle m_idle{};
    PFN_vkDestroySemaphore m_destroy{};
    PFN_vkFreeCommandBuffers m_free{};
    PFN_vkQueueSubmit2 m_submit{};
    PFN_vkCreateComputePipelines m_compute{};
    PFN_vkResetCommandBuffer m_reset{};
    PFN_vkDestroyImage m_destroyImage{};
    PFN_vkDestroyImageView m_destroyView{};
    PFN_vkDestroyPipeline m_destroyPipeline{};
    PFN_vkCreateSwapchainKHR m_createSwapchain{};
};

#endif
