#ifndef HARMONIA_VULKAN_INIT_PHYSICALDEVICE_HPP
#define HARMONIA_VULKAN_INIT_PHYSICALDEVICE_HPP

#include <volk/volk.h>

#include <cstdint>
#include <expected>

namespace harmonia {

struct PhysicalDeviceInfo {
    VkPhysicalDevice device{};
    VkPhysicalDeviceProperties2 properties{};
    VkPhysicalDeviceMemoryProperties memProperties{};
    std::uint32_t graphicsFamily{};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps{};
    bool serSupported = false;                  ///< VK_EXT_ray_tracing_invocation_reorder is available.
    bool dgcSupported = false;                  ///< VK_EXT_device_generated_commands is available.
    bool opacityMicromapSupported = false;      ///< VK_EXT_opacity_micromap is available.
    bool pageableMemorySupported = false;       ///< VK_EXT_pageable_device_local_memory is available.
    bool calibratedTimestampsSupported = false; ///< VK_KHR_calibrated_timestamps is available.
    bool presentIdSupported = false;            ///< VK_KHR_present_id is available.
    bool presentWaitSupported = false;          ///< VK_KHR_present_wait is available.
    bool fifoLatestReadySupported = false;      ///< VK_KHR_present_mode_fifo_latest_ready is available.
};

class PhysicalDevice {
  public:
    [[nodiscard]] static std::expected<PhysicalDeviceInfo, VkResult> select(VkInstance instance, VkSurfaceKHR surface);

  private:
    [[nodiscard]] static bool hasRayTracingSupport(VkPhysicalDevice device);
    [[nodiscard]] static bool hasRequiredExtensions(VkPhysicalDevice device);
    [[nodiscard]] static bool hasExtension(VkPhysicalDevice device, const char* name);
    [[nodiscard]] static bool hasSerSupport(VkPhysicalDevice device);
    [[nodiscard]] static bool hasRayTracingMaintenance1Support(VkPhysicalDevice device);
    [[nodiscard]] static bool hasDgcSupport(VkPhysicalDevice device);
    [[nodiscard]] static bool hasOpacityMicromapSupport(VkPhysicalDevice device);
};

} // namespace harmonia

#endif // HARMONIA_VULKAN_INIT_PHYSICALDEVICE_HPP
