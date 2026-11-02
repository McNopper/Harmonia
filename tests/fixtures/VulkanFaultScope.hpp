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
        s_active = this;
        vkWaitSemaphores = wait;
        vkQueueSubmit2 = submit;
        vkDeviceWaitIdle = idle;
        vkDestroySemaphore = destroy;
        vkFreeCommandBuffers = freeBuffers;
        m_installed = true;
    }
    static VkResult cleanupForTest(VkDevice device) noexcept {
        // Destruction safety net, deliberately outside the subject/oracle counters.
        return s_active != nullptr ? s_active->m_idle(device) : vkDeviceWaitIdle(device);
    }
    bool failWaitOnce = false;
    bool failSubmitOnce = false;
    std::uint32_t idleAllocationFailures = 0;
    bool waitInjected = false, submitInjected = false;
    bool retiredBySubject = false, unsafeSemaphoreDestruction = false, unsafeCommandFree = false;
    bool testRecoveryUsed = false;
    std::uint32_t idleCalls = 0;

  private:
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
        self.m_free(device, pool, count, buffers);
    }
    inline static VulkanFaultScope* s_active{};
    bool m_installed = false;
    VkSemaphore m_waitedSemaphore{};
    PFN_vkWaitSemaphores m_wait{};
    PFN_vkQueueSubmit2 m_submit{};
    PFN_vkDeviceWaitIdle m_idle{};
    PFN_vkDestroySemaphore m_destroy{};
    PFN_vkFreeCommandBuffers m_free{};
};

#endif // TESTS_FIXTURES_VULKANFAULTSCOPE_HPP
