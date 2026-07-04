#pragma once

#include <volk/volk.h>

#include <expected>

struct PhysicalDeviceInfo {
    VkPhysicalDevice device{};
    VkPhysicalDeviceProperties2 properties{};
    VkPhysicalDeviceMemoryProperties memProperties{};
    uint32_t graphicsFamily{};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps{};
    bool serSupported = false; ///< VK_EXT_ray_tracing_invocation_reorder is available.
};

class PhysicalDevice {
  public:
    [[nodiscard]] static std::expected<PhysicalDeviceInfo, VkResult> select(VkInstance instance, VkSurfaceKHR surface);

  private:
    [[nodiscard]] static bool hasRayTracingSupport(VkPhysicalDevice device);
    [[nodiscard]] static bool hasRequiredExtensions(VkPhysicalDevice device);
    [[nodiscard]] static bool hasSerSupport(VkPhysicalDevice device);
};
