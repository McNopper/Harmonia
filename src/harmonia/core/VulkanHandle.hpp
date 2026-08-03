#ifndef HARMONIA_CORE_VULKANHANDLE_HPP
#define HARMONIA_CORE_VULKANHANDLE_HPP

#include <volk/volk.h>

namespace harmonia {

/// Move-only RAII owner for a Vulkan handle destroyed by a device-scoped
/// vkDestroy* command (signature `void(VkDevice, Handle, const VkAllocationCallbacks*)`).
///
/// The destroy command is bound at compile time via the address of the volk-loaded
/// function-pointer global (e.g. `&vkDestroyPipeline`), so there is no per-instance
/// storage overhead: destruction calls `(*DestroyPtr)(m_device, m_handle, nullptr)`.
template <typename HandleT, auto* DestroyPtr> class UniqueHandle {
  public:
    constexpr UniqueHandle() noexcept = default;

    constexpr explicit UniqueHandle(VkDevice device, HandleT handle = VK_NULL_HANDLE) noexcept
        : m_device(device), m_handle(handle) {}

    ~UniqueHandle() noexcept { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : m_device(other.m_device), m_handle(other.m_handle) {
        other.m_handle = VK_NULL_HANDLE;
    }

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset();
            m_device = other.m_device;
            m_handle = other.m_handle;
            other.m_handle = VK_NULL_HANDLE;
        }
        return *this;
    }

    /// Destroy the owned handle now (no-op when empty). The device is retained so the
    /// wrapper may own a fresh handle afterwards; a default-constructed wrapper has no
    /// device and therefore cannot destroy (its reset() is a no-op).
    void reset() noexcept {
        if (m_handle != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE) {
            (*DestroyPtr)(m_device, m_handle, nullptr);
        }
        m_handle = VK_NULL_HANDLE;
    }

    [[nodiscard]] constexpr operator HandleT() const noexcept { return m_handle; }
    [[nodiscard]] constexpr HandleT get() const noexcept { return m_handle; }
    [[nodiscard]] const HandleT* ptr() const noexcept { return &m_handle; }
    [[nodiscard]] HandleT release() noexcept {
        const HandleT h = m_handle;
        m_handle = VK_NULL_HANDLE;
        return h;
    }
    [[nodiscard]] constexpr VkDevice device() const noexcept { return m_device; }
    [[nodiscard]] constexpr bool empty() const noexcept { return m_handle == VK_NULL_HANDLE; }

  private:
    VkDevice m_device{};
    HandleT m_handle{};
};

using UniquePipeline = UniqueHandle<VkPipeline, &vkDestroyPipeline>;
using UniquePipelineLayout = UniqueHandle<VkPipelineLayout, &vkDestroyPipelineLayout>;
using UniqueDescriptorSetLayout = UniqueHandle<VkDescriptorSetLayout, &vkDestroyDescriptorSetLayout>;
using UniqueDescriptorPool = UniqueHandle<VkDescriptorPool, &vkDestroyDescriptorPool>;
using UniqueSampler = UniqueHandle<VkSampler, &vkDestroySampler>;
using UniqueImageView = UniqueHandle<VkImageView, &vkDestroyImageView>;
using UniqueShaderModule = UniqueHandle<VkShaderModule, &vkDestroyShaderModule>;
using UniqueSemaphore = UniqueHandle<VkSemaphore, &vkDestroySemaphore>;
using UniqueFence = UniqueHandle<VkFence, &vkDestroyFence>;
using UniqueCommandPool = UniqueHandle<VkCommandPool, &vkDestroyCommandPool>;
using UniqueAccelerationStructure = UniqueHandle<VkAccelerationStructureKHR, &vkDestroyAccelerationStructureKHR>;
using UniqueMicromapEXT = UniqueHandle<VkMicromapEXT, &vkDestroyMicromapEXT>;
using UniqueSwapchainKHR = UniqueHandle<VkSwapchainKHR, &vkDestroySwapchainKHR>;
using UniqueIndirectCommandsLayout = UniqueHandle<VkIndirectCommandsLayoutEXT, &vkDestroyIndirectCommandsLayoutEXT>;

} // namespace harmonia

#endif // HARMONIA_CORE_VULKANHANDLE_HPP
