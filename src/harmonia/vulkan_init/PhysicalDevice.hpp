#ifndef HARMONIA_VULKAN_INIT_PHYSICALDEVICE_HPP
#define HARMONIA_VULKAN_INIT_PHYSICALDEVICE_HPP

#include <volk/volk.h>

#include <cstdint>
#include <expected>

struct PhysicalDeviceInfo {
    VkPhysicalDevice device{};
    VkPhysicalDeviceProperties2 properties{};
    VkPhysicalDeviceMemoryProperties memProperties{};
    std::uint32_t graphicsFamily{};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps{};
    bool serSupported = false;         ///< VK_EXT_ray_tracing_invocation_reorder is available.
    bool indirectRt2Supported = false; ///< rayTracingPipelineTraceRaysIndirect2 available.
    bool dgcSupported = false;         ///< VK_EXT_device_generated_commands is available.
};

class PhysicalDevice {
  public:
    [[nodiscard]] static std::expected<PhysicalDeviceInfo, VkResult> select(VkInstance instance, VkSurfaceKHR surface);

  private:
    [[nodiscard]] static bool hasRayTracingSupport(VkPhysicalDevice device);
    [[nodiscard]] static bool hasRequiredExtensions(VkPhysicalDevice device);
    [[nodiscard]] static bool hasSerSupport(VkPhysicalDevice device);
    [[nodiscard]] static bool hasRayTracingMaintenance1Support(VkPhysicalDevice device);
    [[nodiscard]] static bool hasDgcSupport(VkPhysicalDevice device);
};
#endif // HARMONIA_VULKAN_INIT_PHYSICALDEVICE_HPP
