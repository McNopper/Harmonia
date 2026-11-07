#include "harmonia/vulkan_init/Context.hpp"

#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <vma/vk_mem_alloc.h>

#include "harmonia/core/Logger.hpp"
#include "harmonia/vulkan_init/Capabilities.hpp"

namespace harmonia::vulkan_init {

namespace {
[[nodiscard]] bool validationEnabled(bool requested) noexcept {
    return requested;
}

[[nodiscard]] bool hasValidationLayer() {
    std::uint32_t layerCount = 0;
    if (vkEnumerateInstanceLayerProperties(&layerCount, nullptr) != VK_SUCCESS) {
        return false;
    }
    std::vector<VkLayerProperties> layers(layerCount);
    if (vkEnumerateInstanceLayerProperties(&layerCount, layers.data()) != VK_SUCCESS) {
        return false;
    }
    for (const VkLayerProperties& layer : layers) {
        if (std::string_view(layer.layerName) == "VK_LAYER_KHRONOS_validation") {
            return true;
        }
    }
    return false;
}

[[nodiscard]] VkResult createSurface(VkInstance instance, SDL_Window* window, VkSurfaceKHR& surface) {
    if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) {
        return VK_ERROR_SURFACE_LOST_KHR;
    }
    return VK_SUCCESS;
}

// Verify required volk device globals were loaded.
[[nodiscard]] VkResult checkRayTracingFunctions() {
    return (vkCmdTraceRaysKHR != nullptr && vkCmdBuildAccelerationStructuresKHR != nullptr &&
            vkCreateAccelerationStructure2KHR != nullptr && vkDestroyAccelerationStructureKHR != nullptr &&
            vkGetRayTracingShaderGroupHandlesKHR != nullptr && vkCmdPushDescriptorSet != nullptr)
               ? VK_SUCCESS
               : VK_ERROR_FEATURE_NOT_PRESENT;
}

[[nodiscard]] VkResult createAllocator(const DeviceContext& ctx, VkInstance instance, VmaAllocator& allocator) {
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    functions.vkGetPhysicalDeviceProperties = vkGetPhysicalDeviceProperties;
    functions.vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties;
    functions.vkAllocateMemory = vkAllocateMemory;
    functions.vkFreeMemory = vkFreeMemory;
    functions.vkMapMemory = vkMapMemory;
    functions.vkUnmapMemory = vkUnmapMemory;
    functions.vkFlushMappedMemoryRanges = vkFlushMappedMemoryRanges;
    functions.vkInvalidateMappedMemoryRanges = vkInvalidateMappedMemoryRanges;
    functions.vkBindBufferMemory = vkBindBufferMemory;
    functions.vkBindImageMemory = vkBindImageMemory;
    functions.vkGetBufferMemoryRequirements = vkGetBufferMemoryRequirements;
    functions.vkGetImageMemoryRequirements = vkGetImageMemoryRequirements;
    functions.vkCreateBuffer = vkCreateBuffer;
    functions.vkDestroyBuffer = vkDestroyBuffer;
    functions.vkCreateImage = vkCreateImage;
    functions.vkDestroyImage = vkDestroyImage;
    functions.vkCmdCopyBuffer = vkCmdCopyBuffer;
    functions.vkGetBufferMemoryRequirements2KHR = vkGetBufferMemoryRequirements2;
    functions.vkGetImageMemoryRequirements2KHR = vkGetImageMemoryRequirements2;
    functions.vkBindBufferMemory2KHR = vkBindBufferMemory2;
    functions.vkBindImageMemory2KHR = vkBindImageMemory2;
    functions.vkGetPhysicalDeviceMemoryProperties2KHR = vkGetPhysicalDeviceMemoryProperties2;
    functions.vkGetDeviceBufferMemoryRequirements = vkGetDeviceBufferMemoryRequirements;
    functions.vkGetDeviceImageMemoryRequirements = vkGetDeviceImageMemoryRequirements;

    // Pageable device-local memory: when supported, let VMA assign per-allocation priorities so
    // the driver can page device-local memory under VRAM pressure (VK_EXT_pageable_device_local_memory
    // depends on VK_EXT_memory_priority, both enabled together in createDevice).
    VmaAllocatorCreateFlags flags =
        VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT | VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE5_BIT;
    if (ctx.pageableMemorySupported) {
        flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT;
    }

    const VmaAllocatorCreateInfo createInfo{
        .flags = flags,
        .physicalDevice = ctx.physicalDevice,
        .device = ctx.device,
        .preferredLargeHeapBlockSize = 0,
        .pAllocationCallbacks = nullptr,
        .pDeviceMemoryCallbacks = nullptr,
        .pHeapSizeLimit = nullptr,
        .pVulkanFunctions = &functions,
        .instance = instance,
        .vulkanApiVersion = VK_API_VERSION_1_4,
        .pTypeExternalMemoryHandleTypes = nullptr,
    };
    return vmaCreateAllocator(&createInfo, &allocator);
}

// The always-on device extensions. Optional capability extensions are appended
// by the probe loop in createDevice (see capabilityProbes), auto-enabled KHR
// maintenance extensions by collectAdvertisedMaintenanceExtensions.
[[nodiscard]] std::vector<const char*> buildBaseExtensionList() {
    std::vector<const char*> deviceExtensions{
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        VK_KHR_RAY_QUERY_EXTENSION_NAME,
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_MAINTENANCE_1_EXTENSION_NAME,
        // MOD5: device-address-only AS creation (vkCreateAccelerationStructure2KHR) — the
        // forward path per the Khronos AS deprecation blog. Hard-required: the legacy
        // vkCreateAccelerationStructureKHR path is dropped from this codebase.
        VK_KHR_DEVICE_ADDRESS_COMMANDS_EXTENSION_NAME,
        // VK14: robustness2 — nullDescriptor lets us bind VK_NULL_HANDLE instead of
        // creating 1×1 dummy images/buffers for optional resources. Probe→enable.
        VK_KHR_ROBUSTNESS_2_EXTENSION_NAME,
        // MOD1: descriptor buffers — the modern GPU-driven descriptor path (replaces pools/sets).
        VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
    };
    // MOD4 v1: VK_KHR_swapchain_maintenance1 (the former VK_EXT_swapchain_maintenance1)
    // is probe-enabled for the fence/release half — per-present fences
    // (VkSwapchainPresentFenceInfoKHR) + image release on resize
    // (vkReleaseSwapchainImagesKHR); see the capabilityProbes row and
    // Swapchain::present/recreate. The present-scaling sub-feature was probed
    // (supportedPresentScaling = 0 for all present modes on the reference GPU,
    // RTX 5070 / driver 621.01) and stays out. Devices without the extension keep
    // the legacy device-idle resize path.
    return deviceExtensions;
}

// Auto-enable policy: every advertised VK_KHR_maintenance<N> device extension is
// enabled. The maintenance semantics are additive and cross-vendor (KHR) by
// definition, so the RHI wants them all available and future API adoption needs
// no probe dance. The result is dedup-safe against the hand-built extension
// list; extensions without a feature struct in the SDK headers are enabled by
// string alone (valid: they are additive).
[[nodiscard]] std::vector<std::string> collectAdvertisedMaintenanceExtensions(VkPhysicalDevice device) {
    std::vector<std::string> extensions;
    std::uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS || count == 0U) {
        return extensions;
    }
    std::vector<VkExtensionProperties> available(count);
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, available.data()) != VK_SUCCESS) {
        return extensions;
    }
    constexpr std::string_view kPrefix = "VK_KHR_maintenance";
    for (const VkExtensionProperties& extension : available) {
        const std::string_view name{extension.extensionName};
        if (!name.starts_with(kPrefix)) {
            continue;
        }
        const std::string_view digits = name.substr(kPrefix.size());
        const bool isNumbered = !digits.empty() && std::all_of(digits.begin(), digits.end(), [](const char digit) {
            return digit >= '0' && digit <= '9';
        });
        if (isNumbered) {
            extensions.emplace_back(name);
        }
    }
    return extensions;
}

[[nodiscard]] VkResult createDevice(const PhysicalDeviceInfo& info, DeviceContext& ctx) {
    const SupportedFeatures supported = querySupportedFeatures(info.device);

    // Optional capability probes (declarative table): the extension(s) are enabled
    // and the capability flag recorded when the extension was advertised at
    // enumerate time AND — where the row says so — the feature bit is TRUE in the
    // supported chain. Exactly the conditions the former hand-written probe→flag
    // blocks enforced.
    CapabilityFlags capabilities{};
    std::vector<const char*> deviceExtensions = buildBaseExtensionList();
    for (const CapabilityProbe& probe : capabilityProbes(info, supported)) {
        const bool extensionAdvertised = probe.infoFlag == nullptr || *probe.infoFlag;
        const bool featurePresent = probe.featureBit == nullptr || *probe.featureBit == VK_TRUE;
        if (!extensionAdvertised || !featurePresent) {
            continue;
        }
        capabilities.*(probe.flag) = true;
        for (const char* extension : probe.extensions) {
            if (extension != nullptr) {
                deviceExtensions.push_back(extension);
            }
        }
    }

    // Auto-enable every advertised KHR maintenance extension (policy comment on
    // collectAdvertisedMaintenanceExtensions): dedup-safe append.
    const std::vector<std::string> maintenanceExtensions = collectAdvertisedMaintenanceExtensions(info.device);
    for (const std::string& extension : maintenanceExtensions) {
        const bool alreadyEnabled = std::any_of(deviceExtensions.begin(),
                                                deviceExtensions.end(),
                                                [&extension](const char* enabled) { return extension == enabled; });
        if (!alreadyEnabled) {
            deviceExtensions.push_back(extension.c_str());
        }
    }

    if (supported.features12.bufferDeviceAddress != VK_TRUE || supported.features12.descriptorIndexing != VK_TRUE ||
        supported.features12.runtimeDescriptorArray != VK_TRUE ||
        supported.features12.descriptorBindingPartiallyBound != VK_TRUE ||
        supported.features12.descriptorBindingStorageBufferUpdateAfterBind != VK_TRUE ||
        supported.features12.descriptorBindingSampledImageUpdateAfterBind != VK_TRUE ||
        supported.features12.descriptorBindingStorageImageUpdateAfterBind != VK_TRUE ||
        supported.features12.timelineSemaphore != VK_TRUE || supported.features13.dynamicRendering != VK_TRUE ||
        supported.features13.synchronization2 != VK_TRUE || supported.features13.maintenance4 != VK_TRUE ||
        supported.features13.shaderDemoteToHelperInvocation != VK_TRUE ||
        supported.features14.pushDescriptor != VK_TRUE || supported.features14.maintenance5 != VK_TRUE ||
        supported.features14.hostImageCopy != VK_TRUE || supported.as.accelerationStructure != VK_TRUE ||
        // MOD5: device-address-only AS creation is hard-required.
        supported.addressCommands.deviceAddressCommands != VK_TRUE ||
        // VK14: robustness2 nullDescriptor is hard-required (dummies are deleted).
        supported.robustness2.nullDescriptor != VK_TRUE ||
        // MOD1: descriptor buffer is hard-required (pools are being migrated).
        supported.descriptorBuffer.descriptorBuffer != VK_TRUE ||
        supported.as.descriptorBindingAccelerationStructureUpdateAfterBind != VK_TRUE ||
        supported.rt.rayTracingPipeline != VK_TRUE || supported.rayQuery.rayQuery != VK_TRUE ||
        supported.rtMaintenance1.rayTracingMaintenance1 != VK_TRUE ||
        supported.rtMaintenance1.rayTracingPipelineTraceRaysIndirect2 != VK_TRUE ||
        supported.features2.features.fragmentStoresAndAtomics != VK_TRUE ||
        supported.features2.features.independentBlend != VK_TRUE) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }

    EnabledFeatures enabled{};
    const void* featuresHead = buildEnabledFeatures(enabled, supported, capabilities, maintenanceExtensions);

    const QueueInfos queues = buildQueueCreateInfos(info.device, info.graphicsFamily);
    const VkDeviceCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = featuresHead,
        .flags = 0,
        .queueCreateInfoCount = static_cast<std::uint32_t>(queues.infos.size()),
        .pQueueCreateInfos = queues.infos.data(),
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = static_cast<std::uint32_t>(deviceExtensions.size()),
        .ppEnabledExtensionNames = deviceExtensions.data(),
        .pEnabledFeatures = nullptr,
    };

    const VkResult result = vkCreateDevice(info.device, &createInfo, nullptr, &ctx.device);
    if (result == VK_SUCCESS) {
        ctx.positionFetchSupported = capabilities.positionFetchSupported;
        ctx.serSupported = capabilities.serSupported;
        ctx.dgcSupported = capabilities.dgcSupported;
        ctx.opacityMicromapSupported = capabilities.opacityMicromapSupported;
        ctx.pageableMemorySupported = capabilities.pageableMemorySupported;
        ctx.calibratedTimestampsSupported = capabilities.calibratedTimestampsSupported;
        ctx.presentIdSupported = capabilities.presentIdSupported;
        ctx.presentWaitSupported = capabilities.presentWaitSupported;
        ctx.fifoLatestReadySupported = capabilities.fifoLatestReadySupported;
        ctx.deviceFaultSupported = capabilities.deviceFaultSupported;
        ctx.addressBindingReportSupported = capabilities.addressBindingReportSupported;
        ctx.swapchainMaintenance1Supported = capabilities.swapchainMaintenance1Supported;
        ctx.memoryBudgetSupported = capabilities.memoryBudgetSupported;
        ctx.presentTimingSupported = capabilities.presentTimingSupported;
        ctx.subgroupRotateSupported = capabilities.subgroupRotateSupported;
        ctx.shaderMaximalReconvergenceSupported = capabilities.shaderMaximalReconvergenceSupported;
        ctx.shaderSubgroupUniformControlFlowSupported = capabilities.shaderSubgroupUniformControlFlowSupported;
        ctx.shaderQuadControlSupported = capabilities.shaderQuadControlSupported;
        ctx.pipelineBinarySupported = capabilities.pipelineBinarySupported;
        ctx.asyncComputeQueueFamily = queues.asyncComputeFamily;
    }
    return result;
}
} // namespace

std::expected<Context, VkResult> Context::create(const Config& config) {
    if (config.window == nullptr) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }
    VkResult result = volkInitialize();
    if (result != VK_SUCCESS) {
        return std::unexpected(result);
    }

    Context context;
    context.m_validationEnabled = validationEnabled(config.enableValidation);

    Uint32 sdlExtensionCount = 0;
    const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);
    if (sdlExtensions == nullptr || sdlExtensionCount == 0U) {
        return std::unexpected(VK_ERROR_EXTENSION_NOT_PRESENT);
    }

    std::vector<const char*> extensions(sdlExtensions, sdlExtensions + sdlExtensionCount);

    // MOD6: VK_KHR_present_id2/present_wait2 depend on VK_KHR_get_surface_capabilities2
    // (an instance extension SDL does not provide).
    extensions.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);

    // Probe available instance extensions so we can opt-in to HDR color spaces.
    // VK_EXT_swapchain_colorspace is required to use any non-sRGB VkColorSpaceKHR
    // (HDR10, HLG, scRGB, Display P3 …) in a swapchain; without it the validation
    // layer rejects vkCreateSwapchainKHR even if the driver enumerates those formats.
    bool debugUtilsInstanceExtension = false;
    {
        std::uint32_t extCount = 0;
        if (vkEnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr) == VK_SUCCESS && extCount > 0) {
            std::vector<VkExtensionProperties> available(extCount);
            vkEnumerateInstanceExtensionProperties(nullptr, &extCount, available.data());
            for (const VkExtensionProperties& ext : available) {
                if (std::string_view(ext.extensionName) == VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME) {
                    extensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
                } else if (std::string_view(ext.extensionName) == VK_EXT_DEBUG_UTILS_EXTENSION_NAME) {
                    // VK16: the address-binding events of VK_EXT_device_address_binding_report
                    // are delivered through the debug-utils messaging mechanism, so the instance
                    // extension must be enabled whenever it may be recorded — not only under
                    // validation (it also makes setDebugName resource naming functional without
                    // validation layers).
                    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                    debugUtilsInstanceExtension = true;
                }
            }
        }
    }

    std::vector<const char*> layers;
    const VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo = DebugUtils::messengerCreateInfo();

    if (context.m_validationEnabled) {
        if (!hasValidationLayer()) {
            return std::unexpected(VK_ERROR_LAYER_NOT_PRESENT);
        }
        layers.push_back("VK_LAYER_KHRONOS_validation");
        if (!debugUtilsInstanceExtension) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
    }

    const VkApplicationInfo appInfo{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext = nullptr,
        .pApplicationName = config.appName.c_str(),
        .applicationVersion = config.appVersion,
        .pEngineName = "harmonia",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_4,
    };
    const VkInstanceCreateInfo instanceInfo{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = context.m_validationEnabled ? &debugCreateInfo : nullptr,
        .flags = 0,
        .pApplicationInfo = &appInfo,
        .enabledLayerCount = static_cast<std::uint32_t>(layers.size()),
        .ppEnabledLayerNames = layers.empty() ? nullptr : layers.data(),
        .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };

    result = vkCreateInstance(&instanceInfo, nullptr, &context.m_instance);
    if (result != VK_SUCCESS) {
        return std::unexpected(result);
    }

    volkLoadInstance(context.m_instance);

    if (context.m_validationEnabled) {
        auto debugUtils = DebugUtils::create(context.m_instance);
        if (!debugUtils) {
            return std::unexpected(debugUtils.error());
        }
        context.m_debugUtils = std::move(*debugUtils);
    }

    result = createSurface(context.m_instance, config.window, context.m_surface);
    if (result != VK_SUCCESS) {
        return std::unexpected(result);
    }

    auto physical = PhysicalDevice::select(context.m_instance, context.m_surface);
    if (!physical) {
        return std::unexpected(physical.error());
    }
    context.m_physicalDeviceInfo = *physical;

    context.m_deviceContext.physicalDevice = context.m_physicalDeviceInfo.device;
    context.m_deviceContext.graphicsFamily = context.m_physicalDeviceInfo.graphicsFamily;

    // VK16: record device address binding events when the feature will be enabled at
    // device creation — the bind/unbind records give a device_fault report's raw buffer
    // addresses their resource context. Diagnosability only: a messenger failure is
    // logged and ignored (no behavior change).
    if (context.m_physicalDeviceInfo.addressBindingReportSupported && debugUtilsInstanceExtension) {
        auto addressBinding = DebugUtils::createAddressBinding(context.m_instance);
        if (addressBinding) {
            context.m_addressBinding = std::move(*addressBinding);
            core::Logger::info("AddressBinding: recording VK_EXT_device_address_binding_report binding events");
        } else {
            core::Logger::warn("AddressBinding: messenger creation failed — binding events will not be recorded");
        }
    }

    result = createDevice(context.m_physicalDeviceInfo, context.m_deviceContext);
    if (result != VK_SUCCESS) {
        return std::unexpected(result);
    }

    volkLoadDevice(context.m_deviceContext.device);
    result = checkRayTracingFunctions();
    if (result != VK_SUCCESS) {
        return std::unexpected(result);
    }

    vkGetDeviceQueue(context.m_deviceContext.device,
                     context.m_physicalDeviceInfo.graphicsFamily,
                     0,
                     &context.m_deviceContext.graphicsQueue);
    if (context.m_deviceContext.asyncComputeQueueFamily != UINT32_MAX) {
        vkGetDeviceQueue(context.m_deviceContext.device,
                         context.m_deviceContext.asyncComputeQueueFamily,
                         0,
                         &context.m_deviceContext.asyncComputeQueue);
    }
    result = createAllocator(context.m_deviceContext, context.m_instance, context.m_deviceContext.allocator);
    if (result != VK_SUCCESS) {
        return std::unexpected(result);
    }

    return context;
}

} // namespace harmonia::vulkan_init
