#include "harmonia/vulkan_init/PhysicalDevice.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace harmonia {

namespace {
constexpr std::array kRequiredExtensions{
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
};

[[nodiscard]] std::int32_t scoreDevice(VkPhysicalDeviceProperties properties) noexcept {
    std::int32_t score = 0;
    if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
        score += 1000;
    } else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
        score += 250;
    }
    score += static_cast<std::int32_t>(properties.limits.maxImageDimension2D);
    return score;
}

struct PresentExtensionSupport {
    bool presentId = false;
    bool presentWait = false;
    bool fifoLatestReady = false;
};

// Probes the present-pacing extension trio (present_id / present_wait /
// present_mode_fifo_latest_ready): one enumerate + one feature query.
[[nodiscard]] PresentExtensionSupport queryPresentExtensions(VkPhysicalDevice device) {
    PresentExtensionSupport out;
    std::uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS || count == 0U) {
        return out;
    }
    std::vector<VkExtensionProperties> exts(count);
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, exts.data()) != VK_SUCCESS) {
        return out;
    }
    const auto has = [&exts](const char* name) {
        for (const VkExtensionProperties& e : exts) {
            if (std::string_view(e.extensionName) == name) {
                return true;
            }
        }
        return false;
    };
    const bool extId = has(VK_KHR_PRESENT_ID_EXTENSION_NAME);
    const bool extWait = has(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    const bool extFlr = has(VK_KHR_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME);
    if (!extId && !extWait && !extFlr) {
        return out;
    }

    VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR flr{};
    flr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_MODE_FIFO_LATEST_READY_FEATURES_KHR;
    VkPhysicalDevicePresentWaitFeaturesKHR wait{};
    wait.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR;
    wait.pNext = extFlr ? &flr : nullptr;
    VkPhysicalDevicePresentIdFeaturesKHR id{};
    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
    id.pNext = extWait ? &wait : nullptr;
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = extId ? &id : nullptr;
    vkGetPhysicalDeviceFeatures2(device, &features2);

    out.presentId = extId && id.presentId == VK_TRUE;
    out.presentWait = extWait && wait.presentWait == VK_TRUE;
    out.fifoLatestReady = extFlr && flr.presentModeFifoLatestReady == VK_TRUE;
    return out;
}
} // namespace

std::expected<PhysicalDeviceInfo, VkResult> PhysicalDevice::select(VkInstance instance, VkSurfaceKHR surface) {
    std::uint32_t deviceCount = 0;
    VkResult result = vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (result != VK_SUCCESS) {
        return std::unexpected(result);
    }
    if (deviceCount == 0U) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    result = vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
    if (result != VK_SUCCESS) {
        return std::unexpected(result);
    }

    bool foundCompatible = false;
    std::int32_t bestScore = -1;
    PhysicalDeviceInfo bestInfo{};

    for (VkPhysicalDevice device : devices) {
        if (!hasRequiredExtensions(device) || !hasRayTracingSupport(device) ||
            !hasRayTracingMaintenance1Support(device)) {
            continue;
        }

        std::uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
        if (queueFamilyCount == 0U) {
            continue;
        }

        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

        std::uint32_t graphicsFamily = VK_QUEUE_FAMILY_IGNORED;
        for (std::uint32_t i = 0; i < queueFamilyCount; ++i) {
            if ((queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0U) {
                continue;
            }

            VkBool32 presentSupported = VK_FALSE;
            result = vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupported);
            if (result != VK_SUCCESS) {
                return std::unexpected(result);
            }
            if (presentSupported == VK_TRUE) {
                graphicsFamily = i;
                break;
            }
        }
        if (graphicsFamily == VK_QUEUE_FAMILY_IGNORED) {
            continue;
        }

        PhysicalDeviceInfo info{};
        info.device = device;
        info.graphicsFamily = graphicsFamily;
        info.rtProps = {};
        info.rtProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
        info.properties = {};
        info.properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        info.properties.pNext = &info.rtProps;
        vkGetPhysicalDeviceProperties2(device, &info.properties);
        vkGetPhysicalDeviceMemoryProperties(device, &info.memProperties);
        info.serSupported = hasSerSupport(device);
        info.dgcSupported = hasDgcSupport(device);
        info.opacityMicromapSupported = hasOpacityMicromapSupport(device);
        info.pageableMemorySupported = hasExtension(device, VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME);
        info.calibratedTimestampsSupported = hasExtension(device, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
        const PresentExtensionSupport present = queryPresentExtensions(device);
        info.presentIdSupported = present.presentId;
        info.presentWaitSupported = present.presentWait;
        info.fifoLatestReadySupported = present.fifoLatestReady;

        const std::int32_t score = scoreDevice(info.properties.properties) + 500;
        if (!foundCompatible || score > bestScore) {
            foundCompatible = true;
            bestScore = score;
            bestInfo = info;
        }
    }

    if (!foundCompatible) {
        return std::unexpected(VK_ERROR_FEATURE_NOT_PRESENT);
    }
    return bestInfo;
}

bool PhysicalDevice::hasRayTracingSupport(VkPhysicalDevice device) {
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtFeatures{};
    rtFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures{};
    asFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    asFeatures.pNext = &rtFeatures;
    VkPhysicalDeviceVulkan14Features features14{};
    features14.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
    features14.pNext = &asFeatures;
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.pNext = &features14;
    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.pNext = &features13;
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &features12;
    vkGetPhysicalDeviceFeatures2(device, &features2);

    return asFeatures.accelerationStructure == VK_TRUE && rtFeatures.rayTracingPipeline == VK_TRUE &&
           features12.bufferDeviceAddress == VK_TRUE && features12.descriptorIndexing == VK_TRUE &&
           features12.runtimeDescriptorArray == VK_TRUE && features12.descriptorBindingPartiallyBound == VK_TRUE &&
           features12.descriptorBindingStorageBufferUpdateAfterBind == VK_TRUE &&
           features12.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE &&
           features13.dynamicRendering == VK_TRUE && features13.synchronization2 == VK_TRUE &&
           features13.maintenance4 == VK_TRUE && features14.pushDescriptor == VK_TRUE;
}

bool PhysicalDevice::hasSerSupport(VkPhysicalDevice device) {
    std::uint32_t extensionCount = 0;
    VkResult result = vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
    if (result != VK_SUCCESS || extensionCount == 0U) {
        return false;
    }

    std::vector<VkExtensionProperties> extensions(extensionCount);
    result = vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data());
    if (result != VK_SUCCESS) {
        return false;
    }

    bool extensionAvailable = false;
    for (const VkExtensionProperties& extension : extensions) {
        if (std::string_view(extension.extensionName) == VK_EXT_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME) {
            extensionAvailable = true;
            break;
        }
    }
    if (!extensionAvailable) {
        return false;
    }

    VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT serFeatures{};
    serFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_EXT;
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &serFeatures;
    vkGetPhysicalDeviceFeatures2(device, &features2);

    return serFeatures.rayTracingInvocationReorder == VK_TRUE;
}

bool PhysicalDevice::hasRayTracingMaintenance1Support(VkPhysicalDevice device) {
    std::uint32_t extensionCount = 0;
    VkResult result = vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
    if (result != VK_SUCCESS || extensionCount == 0U) {
        return false;
    }

    std::vector<VkExtensionProperties> extensions(extensionCount);
    result = vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data());
    if (result != VK_SUCCESS) {
        return false;
    }

    bool extensionAvailable = false;
    for (const VkExtensionProperties& extension : extensions) {
        if (std::string_view(extension.extensionName) == VK_KHR_RAY_TRACING_MAINTENANCE_1_EXTENSION_NAME) {
            extensionAvailable = true;
            break;
        }
    }
    if (!extensionAvailable) {
        return false;
    }

    VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR maint1Features{};
    maint1Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MAINTENANCE_1_FEATURES_KHR;
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &maint1Features;
    vkGetPhysicalDeviceFeatures2(device, &features2);

    return maint1Features.rayTracingMaintenance1 == VK_TRUE &&
           maint1Features.rayTracingPipelineTraceRaysIndirect2 == VK_TRUE;
}

bool PhysicalDevice::hasExtension(VkPhysicalDevice device, const char* name) {
    std::uint32_t extensionCount = 0;
    VkResult result = vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
    if (result != VK_SUCCESS || extensionCount == 0U) {
        return false;
    }

    std::vector<VkExtensionProperties> extensions(extensionCount);
    result = vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data());
    if (result != VK_SUCCESS) {
        return false;
    }

    for (const VkExtensionProperties& extension : extensions) {
        if (std::string_view(extension.extensionName) == name) {
            return true;
        }
    }
    return false;
}

bool PhysicalDevice::hasRequiredExtensions(VkPhysicalDevice device) {
    std::uint32_t extensionCount = 0;
    VkResult result = vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
    if (result != VK_SUCCESS || extensionCount == 0U) {
        return false;
    }

    std::vector<VkExtensionProperties> extensions(extensionCount);
    result = vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data());
    if (result != VK_SUCCESS) {
        return false;
    }

    for (const char* required : kRequiredExtensions) {
        bool found = false;
        for (const VkExtensionProperties& extension : extensions) {
            if (std::string_view(extension.extensionName) == required) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

bool PhysicalDevice::hasDgcSupport(VkPhysicalDevice device) {
    std::uint32_t extensionCount = 0;
    VkResult result = vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
    if (result != VK_SUCCESS || extensionCount == 0U) {
        return false;
    }

    std::vector<VkExtensionProperties> extensions(extensionCount);
    result = vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data());
    if (result != VK_SUCCESS) {
        return false;
    }

    bool extensionAvailable = false;
    for (const VkExtensionProperties& extension : extensions) {
        if (std::string_view(extension.extensionName) == VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME) {
            extensionAvailable = true;
            break;
        }
    }
    if (!extensionAvailable) {
        return false;
    }

    VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT dgcFeatures{};
    dgcFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT;
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &dgcFeatures;
    vkGetPhysicalDeviceFeatures2(device, &features2);

    return dgcFeatures.deviceGeneratedCommands == VK_TRUE;
}

bool PhysicalDevice::hasOpacityMicromapSupport(VkPhysicalDevice device) {
    std::uint32_t extensionCount = 0;
    VkResult result = vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
    if (result != VK_SUCCESS || extensionCount == 0U) {
        return false;
    }

    std::vector<VkExtensionProperties> extensions(extensionCount);
    result = vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data());
    if (result != VK_SUCCESS) {
        return false;
    }

    bool extensionAvailable = false;
    for (const VkExtensionProperties& extension : extensions) {
        if (std::string_view(extension.extensionName) == VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME) {
            extensionAvailable = true;
            break;
        }
    }
    if (!extensionAvailable) {
        return false;
    }

    VkPhysicalDeviceOpacityMicromapFeaturesEXT ommFeatures{};
    ommFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT;
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &ommFeatures;
    vkGetPhysicalDeviceFeatures2(device, &features2);

    return ommFeatures.micromap == VK_TRUE;
}

} // namespace harmonia
