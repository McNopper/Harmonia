#ifndef HARMONIA_DEVICECONTEXT_HPP
#define HARMONIA_DEVICECONTEXT_HPP

#include <volk/volk.h>

#include <cstdint>
#include <vma/vk_mem_alloc.h>

namespace harmonia {

/// Minimal Vulkan device state shared across modules.
/// After volkLoadDevice() the codebase calls Vulkan entry points through volk globals,
/// so no per-context pfn fields are stored here.
struct DeviceContext {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VmaAllocator allocator = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    std::uint32_t graphicsFamily = 0;
    bool positionFetchSupported = false;  ///< VK_KHR_ray_tracing_position_fetch is enabled.
    bool serSupported = false;            ///< VK_EXT_ray_tracing_invocation_reorder is enabled.
    bool dgcSupported = false;            ///< VK_EXT_device_generated_commands is enabled.
    bool pageableMemorySupported = false; ///< VK_EXT_pageable_device_local_memory enabled (driver auto-uses paging).
    bool calibratedTimestampsSupported =
        false;                             ///< VK_KHR_calibrated_timestamps enabled (GPU/CPU timestamp correlation).
    bool presentIdSupported = false;       ///< VK_KHR_present_id enabled (per-present ID tagging).
    bool presentWaitSupported = false;     ///< VK_KHR_present_wait enabled (vkWaitForPresentKHR pacing).
    bool fifoLatestReadySupported = false; ///< VK_KHR_present_mode_fifo_latest_ready enabled (low-latency FIFO).

    /// Dedicated async compute queue (COMPUTE but not GRAPHICS).
    /// VK_NULL_HANDLE when the device has no dedicated compute queue family.
    VkQueue asyncComputeQueue = VK_NULL_HANDLE;
    std::uint32_t asyncComputeQueueFamily = UINT32_MAX;

    [[nodiscard]] bool isValid() const noexcept { return device != VK_NULL_HANDLE; }
    [[nodiscard]] bool hasAsyncCompute() const noexcept { return asyncComputeQueue != VK_NULL_HANDLE; }

    template <typename VkHandle>
    void setDebugName(VkObjectType type, VkHandle handle, const char* name) const noexcept {
        setDebugNameImpl(type, reinterpret_cast<std::uint64_t>(handle), name);
    }

  private:
    void setDebugNameImpl(VkObjectType type, std::uint64_t handle, const char* name) const noexcept {
        if (device == VK_NULL_HANDLE || handle == 0U || name == nullptr || name[0] == '\0' ||
            vkSetDebugUtilsObjectNameEXT == nullptr) {
            return;
        }

        const VkDebugUtilsObjectNameInfoEXT info{
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
            .pNext = nullptr,
            .objectType = type,
            .objectHandle = handle,
            .pObjectName = name,
        };

        static_cast<void>(vkSetDebugUtilsObjectNameEXT(device, &info));
    }
};

} // namespace harmonia

#endif // HARMONIA_DEVICECONTEXT_HPP
