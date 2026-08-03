#include "harmonia/vulkan_init/Context.hpp"

#include <SDL3/SDL_vulkan.h>

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>
#include <vma/vk_mem_alloc.h>

namespace harmonia {

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
            vkCreateAccelerationStructureKHR != nullptr && vkDestroyAccelerationStructureKHR != nullptr &&
            vkGetAccelerationStructureDeviceAddressKHR != nullptr && vkGetRayTracingShaderGroupHandlesKHR != nullptr &&
            vkCmdPushDescriptorSet != nullptr)
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

// ── Device creation helpers (anonymous-namespace; pure builders) ─────────────
// createDevice is a free function, so its extracted steps stay free functions
// here too (no member access needed). The pNext chains require careful lifetime
// management: the *supported* chain only needs to live during the query call, so
// querySupportedFeatures may return by value; the *enabled* chain must outlive
// vkCreateDevice, so buildEnabledFeatures fills storage owned by its caller and
// returns the chain head pointer.

struct SupportedFeatures {
    VkPhysicalDeviceRayQueryFeaturesKHR rayQuery{};
    VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR rtMaintenance1{};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt{};
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh{};
    VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT ser{};
    VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR positionFetch{};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as{};
    VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT dgc{};
    VkPhysicalDeviceOpacityMicromapFeaturesEXT omm{};
    VkPhysicalDeviceMemoryPriorityFeaturesEXT memoryPriority{};
    VkPhysicalDevicePresentIdFeaturesKHR presentId{};
    VkPhysicalDevicePresentWaitFeaturesKHR presentWait{};
    VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR fifoLatestReady{};
    VkPhysicalDeviceVulkan14Features features14{};
    VkPhysicalDeviceVulkan13Features features13{};
    VkPhysicalDeviceVulkan12Features features12{};
    VkPhysicalDeviceVulkan11Features features11{};
    VkPhysicalDeviceFeatures2 features2{};
};

[[nodiscard]] SupportedFeatures querySupportedFeatures(VkPhysicalDevice device) {
    SupportedFeatures s{};
    s.rayQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    s.rtMaintenance1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MAINTENANCE_1_FEATURES_KHR;
    s.rtMaintenance1.pNext = &s.rayQuery;
    s.rt.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    s.rt.pNext = &s.rtMaintenance1;
    s.mesh.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
    s.mesh.pNext = &s.rt;
    s.ser.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_EXT;
    s.ser.pNext = &s.mesh;
    s.positionFetch.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR;
    s.positionFetch.pNext = &s.ser;
    s.as.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    s.as.pNext = &s.positionFetch;
    s.dgc.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT;
    s.dgc.pNext = &s.omm;
    s.omm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT;
    s.omm.pNext = &s.as;
    s.features14.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
    s.features14.pNext = &s.dgc;
    s.features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    s.features13.pNext = &s.features14;
    s.features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    s.features12.pNext = &s.features13;
    s.features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    s.features11.pNext = &s.features12;
    s.features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    s.features2.pNext = &s.features11;
    s.memoryPriority.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT;
    s.rayQuery.pNext = &s.memoryPriority;
    s.presentId.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
    s.memoryPriority.pNext = &s.presentId;
    s.presentWait.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR;
    s.presentId.pNext = &s.presentWait;
    s.fifoLatestReady.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_MODE_FIFO_LATEST_READY_FEATURES_KHR;
    s.presentWait.pNext = &s.fifoLatestReady;
    vkGetPhysicalDeviceFeatures2(device, &s.features2);
    return s;
}

struct EnabledFeatures {
    VkPhysicalDeviceRayQueryFeaturesKHR rayQuery{};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt{};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as{};
    VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT ser{};
    VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR rtMaintenance1{};
    VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR positionFetch{};
    VkPhysicalDeviceVulkan14Features features14{};
    VkPhysicalDeviceVulkan13Features features13{};
    VkPhysicalDeviceVulkan12Features features12{};
    VkPhysicalDeviceVulkan11Features features11{};
    VkPhysicalDeviceFeatures2 features2{};
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh{};
    VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT dgc{};
    VkPhysicalDeviceOpacityMicromapFeaturesEXT omm{};
    VkPhysicalDeviceMemoryPriorityFeaturesEXT memoryPriority{};
    VkPhysicalDevicePresentIdFeaturesKHR presentId{};
    VkPhysicalDevicePresentWaitFeaturesKHR presentWait{};
    VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR fifoLatestReady{};
};

// Fills @p features (caller-owned so the pNext chain survives vkCreateDevice) and
// returns the head pointer to thread into VkDeviceCreateInfo::pNext.
[[nodiscard]] const void* buildEnabledFeatures(EnabledFeatures& features,
                                               const SupportedFeatures& supported,
                                               bool serSupported,
                                               bool positionFetchSupported,
                                               bool meshShaderSupported,
                                               bool dgcSupported,
                                               bool opacityMicromapSupported,
                                               bool pageableMemorySupported,
                                               bool presentIdSupported,
                                               bool presentWaitSupported,
                                               bool fifoLatestReadySupported) {
    features.rayQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    features.rayQuery.rayQuery = VK_TRUE;

    features.rt.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    features.rt.pNext = &features.omm;
    features.rt.rayTracingPipeline = VK_TRUE;

    // VK_EXT_opacity_micromap: per-microtriangle opacity for alpha-tested
    // geometry, resolved by RT traversal (no any-hit shader). Optional; enabled
    // only when the device advertises it. Sits between rt and rayQuery in the
    // always-on core so the optional present/pageable tail still hangs off
    // rayQuery unchanged.
    features.omm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT;
    features.omm.pNext = &features.rayQuery;
    features.omm.micromap = opacityMicromapSupported ? VK_TRUE : VK_FALSE;

    features.as.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    features.as.pNext = &features.rt;
    features.as.accelerationStructure = VK_TRUE;
    features.as.descriptorBindingAccelerationStructureUpdateAfterBind = VK_TRUE;

    features.ser.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_EXT;
    features.ser.pNext = &features.as;
    features.ser.rayTracingInvocationReorder = serSupported ? VK_TRUE : VK_FALSE;

    features.rtMaintenance1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MAINTENANCE_1_FEATURES_KHR;
    features.rtMaintenance1.pNext = &features.ser;
    features.rtMaintenance1.rayTracingMaintenance1 = VK_TRUE;
    features.rtMaintenance1.rayTracingPipelineTraceRaysIndirect2 = VK_TRUE;

    features.positionFetch.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR;
    features.positionFetch.pNext = &features.rtMaintenance1;
    features.positionFetch.rayTracingPositionFetch = positionFetchSupported ? VK_TRUE : VK_FALSE;

    features.features14.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
    features.features14.pNext = &features.positionFetch;
    features.features14.pushDescriptor = VK_TRUE;
    features.features14.maintenance5 = VK_TRUE;
    features.features14.hostImageCopy = VK_TRUE;

    features.features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features.features13.pNext = &features.features14;
    features.features13.synchronization2 = VK_TRUE;
    features.features13.dynamicRendering = VK_TRUE;
    features.features13.maintenance4 = VK_TRUE;

    features.features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features.features12.pNext = &features.features13;
    features.features12.descriptorIndexing = VK_TRUE;
    features.features12.shaderSampledImageArrayNonUniformIndexing =
        supported.features12.shaderSampledImageArrayNonUniformIndexing;
    features.features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    features.features12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
    features.features12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
    features.features12.descriptorBindingPartiallyBound = VK_TRUE;
    features.features12.runtimeDescriptorArray = VK_TRUE;
    features.features12.bufferDeviceAddress = VK_TRUE;
    features.features12.timelineSemaphore = VK_TRUE;

    features.features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    features.features11.pNext = &features.features12;
    features.features11.multiview = supported.features11.multiview;
    features.features11.shaderDrawParameters = supported.features11.shaderDrawParameters;

    features.features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.features2.pNext = &features.features11;
    features.features2.features.multiDrawIndirect = supported.features2.features.multiDrawIndirect;
    features.features2.features.samplerAnisotropy = supported.features2.features.samplerAnisotropy;
    features.features2.features.shaderInt64 = supported.features2.features.shaderInt64;
    features.features2.features.fragmentStoresAndAtomics = VK_TRUE;
    features.features2.features.independentBlend = VK_TRUE;

    // Mesh/task shaders are optional: required by Theia's rasterizer, unused by Hyperion's
    // path tracer. Enable them only when the device advertises support so the shared device
    // creation works on GPUs without VK_EXT_mesh_shader.
    features.mesh.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
    features.mesh.pNext = &features.features2;
    features.mesh.meshShader = VK_TRUE;
    features.mesh.taskShader = supported.mesh.taskShader;

    // VK_EXT_device_generated_commands: GPU-driven mesh/compute draw commands.
    // Optional: enabled when the device supports it so both Theia and Hyperion can use
    // the same device. Falls back to indirect draw when not available.
    features.dgc.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT;
    features.dgc.pNext =
        meshShaderSupported ? static_cast<void*>(&features.mesh) : static_cast<void*>(&features.features2);
    features.dgc.deviceGeneratedCommands = VK_TRUE;

    // VK_EXT_pageable_device_local_memory (depends on VK_EXT_memory_priority): lets the driver
    // page device-local memory by priority. The two extensions + the memoryPriority feature are
    // enabled together so VMA can assign per-allocation priorities (VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT on the
    // allocator).
    features.memoryPriority.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT;
    features.memoryPriority.memoryPriority = pageableMemorySupported ? VK_TRUE : VK_FALSE;

    // Present-pacing trio (present_id / present_wait / present_mode_fifo_latest_ready): each is
    // optional; only link a feature struct into the pNext chain when its extension is enabled.
    features.presentId.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
    features.presentId.presentId = presentIdSupported ? VK_TRUE : VK_FALSE;
    features.presentWait.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR;
    features.presentWait.presentWait = presentWaitSupported ? VK_TRUE : VK_FALSE;
    features.fifoLatestReady.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_MODE_FIFO_LATEST_READY_FEATURES_KHR;
    features.fifoLatestReady.presentModeFifoLatestReady = fifoLatestReadySupported ? VK_TRUE : VK_FALSE;

    // Link the optional tail chain in order (memoryPriority → presentId → presentWait → fifoLatestReady);
    // each enabled node points to the next enabled node, rayQuery (always present) heads it.
    features.fifoLatestReady.pNext = nullptr;
    features.presentWait.pNext = fifoLatestReadySupported ? static_cast<void*>(&features.fifoLatestReady) : nullptr;
    features.presentId.pNext =
        presentWaitSupported ? static_cast<void*>(&features.presentWait)
                             : (fifoLatestReadySupported ? static_cast<void*>(&features.fifoLatestReady) : nullptr);
    features.memoryPriority.pNext =
        presentIdSupported
            ? static_cast<void*>(&features.presentId)
            : (presentWaitSupported
                   ? static_cast<void*>(&features.presentWait)
                   : (fifoLatestReadySupported ? static_cast<void*>(&features.fifoLatestReady) : nullptr));
    features.rayQuery.pNext =
        pageableMemorySupported
            ? static_cast<void*>(&features.memoryPriority)
            : (presentIdSupported
                   ? static_cast<void*>(&features.presentId)
                   : (presentWaitSupported
                          ? static_cast<void*>(&features.presentWait)
                          : (fifoLatestReadySupported ? static_cast<void*>(&features.fifoLatestReady) : nullptr)));

    return dgcSupported          ? static_cast<const void*>(&features.dgc)
           : meshShaderSupported ? static_cast<const void*>(&features.mesh)
                                 : static_cast<const void*>(&features.features2);
}

struct QueueInfos {
    std::vector<VkDeviceQueueCreateInfo> infos;
    std::uint32_t asyncComputeFamily = UINT32_MAX;
};

[[nodiscard]] QueueInfos buildQueueCreateInfos(VkPhysicalDevice device, std::uint32_t graphicsFamily) {
    // static so the create-info pQueuePriorities pointers stay valid after return.
    static constexpr float queuePriority = 1.0f;
    static constexpr float asyncQueuePriority = 0.5f;

    QueueInfos result;
    // Detect a dedicated async compute queue family (COMPUTE without GRAPHICS).
    {
        std::uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueProps(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueProps.data());
        for (std::uint32_t i = 0; i < queueFamilyCount; ++i) {
            const auto& props = queueProps[i];
            if ((props.queueFlags & VK_QUEUE_COMPUTE_BIT) && !(props.queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                props.queueCount > 0) {
                result.asyncComputeFamily = i;
                break;
            }
        }
    }

    result.infos.push_back({
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .queueFamilyIndex = graphicsFamily,
        .queueCount = 1,
        .pQueuePriorities = &queuePriority,
    });
    if (result.asyncComputeFamily != UINT32_MAX) {
        result.infos.push_back({
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .queueFamilyIndex = result.asyncComputeFamily,
            .queueCount = 1,
            .pQueuePriorities = &asyncQueuePriority,
        });
    }
    return result;
}

[[nodiscard]] std::vector<const char*> buildExtensionList(bool serSupported,
                                                          bool positionFetchSupported,
                                                          bool meshShaderSupported,
                                                          bool dgcSupported,
                                                          bool opacityMicromapSupported,
                                                          bool pageableMemorySupported,
                                                          bool calibratedTimestampsSupported,
                                                          bool presentIdSupported,
                                                          bool presentWaitSupported,
                                                          bool fifoLatestReadySupported) {
    std::vector<const char*> deviceExtensions{
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        VK_KHR_RAY_QUERY_EXTENSION_NAME,
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    };
    if (serSupported) {
        deviceExtensions.push_back(VK_EXT_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME);
    }
    deviceExtensions.push_back(VK_KHR_RAY_TRACING_MAINTENANCE_1_EXTENSION_NAME);
    if (positionFetchSupported) {
        deviceExtensions.push_back(VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME);
    }
    if (meshShaderSupported) {
        deviceExtensions.push_back(VK_EXT_MESH_SHADER_EXTENSION_NAME);
    }
    if (dgcSupported) {
        deviceExtensions.push_back(VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME);
    }
    if (opacityMicromapSupported) {
        deviceExtensions.push_back(VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME);
    }
    if (pageableMemorySupported) {
        deviceExtensions.push_back(VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME);
        deviceExtensions.push_back(VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME);
    }
    if (calibratedTimestampsSupported) {
        deviceExtensions.push_back(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
    }
    if (presentIdSupported) {
        deviceExtensions.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
    }
    if (presentWaitSupported) {
        deviceExtensions.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    }
    if (fifoLatestReadySupported) {
        deviceExtensions.push_back(VK_KHR_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME);
    }
    return deviceExtensions;
}

[[nodiscard]] VkResult createDevice(const PhysicalDeviceInfo& info, DeviceContext& ctx) {
    const SupportedFeatures supported = querySupportedFeatures(info.device);

    const bool dgcSupported = info.dgcSupported && supported.dgc.deviceGeneratedCommands == VK_TRUE;
    const bool opacityMicromapSupported = info.opacityMicromapSupported && supported.omm.micromap == VK_TRUE;
    const bool serSupported = supported.ser.rayTracingInvocationReorder == VK_TRUE;
    const bool positionFetchSupported = supported.positionFetch.rayTracingPositionFetch == VK_TRUE;
    const bool meshShaderSupported = supported.mesh.meshShader == VK_TRUE;
    const bool pageableMemorySupported = info.pageableMemorySupported;
    const bool calibratedTimestampsSupported = info.calibratedTimestampsSupported;
    const bool presentIdSupported = info.presentIdSupported && supported.presentId.presentId == VK_TRUE;
    const bool presentWaitSupported = info.presentWaitSupported && supported.presentWait.presentWait == VK_TRUE;
    const bool fifoLatestReadySupported =
        info.fifoLatestReadySupported && supported.fifoLatestReady.presentModeFifoLatestReady == VK_TRUE;

    if (supported.features12.bufferDeviceAddress != VK_TRUE || supported.features12.descriptorIndexing != VK_TRUE ||
        supported.features12.runtimeDescriptorArray != VK_TRUE ||
        supported.features12.descriptorBindingPartiallyBound != VK_TRUE ||
        supported.features12.descriptorBindingStorageBufferUpdateAfterBind != VK_TRUE ||
        supported.features12.descriptorBindingSampledImageUpdateAfterBind != VK_TRUE ||
        supported.features12.descriptorBindingStorageImageUpdateAfterBind != VK_TRUE ||
        supported.features12.timelineSemaphore != VK_TRUE || supported.features13.dynamicRendering != VK_TRUE ||
        supported.features13.synchronization2 != VK_TRUE || supported.features13.maintenance4 != VK_TRUE ||
        supported.features14.pushDescriptor != VK_TRUE || supported.features14.maintenance5 != VK_TRUE ||
        supported.features14.hostImageCopy != VK_TRUE || supported.as.accelerationStructure != VK_TRUE ||
        supported.as.descriptorBindingAccelerationStructureUpdateAfterBind != VK_TRUE ||
        supported.rt.rayTracingPipeline != VK_TRUE || supported.rayQuery.rayQuery != VK_TRUE ||
        supported.rtMaintenance1.rayTracingMaintenance1 != VK_TRUE ||
        supported.rtMaintenance1.rayTracingPipelineTraceRaysIndirect2 != VK_TRUE ||
        supported.features2.features.fragmentStoresAndAtomics != VK_TRUE ||
        supported.features2.features.independentBlend != VK_TRUE) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }

    EnabledFeatures enabled{};
    const void* featuresHead = buildEnabledFeatures(enabled,
                                                    supported,
                                                    serSupported,
                                                    positionFetchSupported,
                                                    meshShaderSupported,
                                                    dgcSupported,
                                                    opacityMicromapSupported,
                                                    pageableMemorySupported,
                                                    presentIdSupported,
                                                    presentWaitSupported,
                                                    fifoLatestReadySupported);

    const QueueInfos queues = buildQueueCreateInfos(info.device, info.graphicsFamily);
    const std::vector<const char*> deviceExtensions = buildExtensionList(serSupported,
                                                                         positionFetchSupported,
                                                                         meshShaderSupported,
                                                                         dgcSupported,
                                                                         opacityMicromapSupported,
                                                                         pageableMemorySupported,
                                                                         calibratedTimestampsSupported,
                                                                         presentIdSupported,
                                                                         presentWaitSupported,
                                                                         fifoLatestReadySupported);
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
        ctx.positionFetchSupported = positionFetchSupported;
        ctx.serSupported = serSupported;
        ctx.dgcSupported = dgcSupported;
        ctx.opacityMicromapSupported = opacityMicromapSupported;
        ctx.pageableMemorySupported = pageableMemorySupported;
        ctx.calibratedTimestampsSupported = calibratedTimestampsSupported;
        ctx.presentIdSupported = presentIdSupported;
        ctx.presentWaitSupported = presentWaitSupported;
        ctx.fifoLatestReadySupported = fifoLatestReadySupported;
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

    // Probe available instance extensions so we can opt-in to HDR color spaces.
    // VK_EXT_swapchain_colorspace is required to use any non-sRGB VkColorSpaceKHR
    // (HDR10, HLG, scRGB, Display P3 …) in a swapchain; without it the validation
    // layer rejects vkCreateSwapchainKHR even if the driver enumerates those formats.
    {
        std::uint32_t extCount = 0;
        if (vkEnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr) == VK_SUCCESS && extCount > 0) {
            std::vector<VkExtensionProperties> available(extCount);
            vkEnumerateInstanceExtensionProperties(nullptr, &extCount, available.data());
            for (const VkExtensionProperties& ext : available) {
                if (std::string_view(ext.extensionName) == VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME) {
                    extensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
                    break;
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
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
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

Context::Context(Context&& other) noexcept
    : m_instance(std::exchange(other.m_instance, VK_NULL_HANDLE)),
      m_surface(std::exchange(other.m_surface, VK_NULL_HANDLE)),
      m_debugUtils(std::move(other.m_debugUtils)),
      m_physicalDeviceInfo(other.m_physicalDeviceInfo),
      m_deviceContext(other.m_deviceContext),
      m_validationEnabled(other.m_validationEnabled) {
    other.m_physicalDeviceInfo = {};
    other.m_deviceContext = {};
    other.m_validationEnabled = false;
}

Context& Context::operator=(Context&& other) noexcept {
    if (this != &other) {
        destroy();
        m_instance = std::exchange(other.m_instance, VK_NULL_HANDLE);
        m_surface = std::exchange(other.m_surface, VK_NULL_HANDLE);
        m_debugUtils = std::move(other.m_debugUtils);
        m_physicalDeviceInfo = other.m_physicalDeviceInfo;
        m_deviceContext = other.m_deviceContext;
        m_validationEnabled = other.m_validationEnabled;
        other.m_physicalDeviceInfo = {};
        other.m_deviceContext = {};
        other.m_validationEnabled = false;
    }
    return *this;
}

Context::~Context() {
    destroy();
}

const DeviceContext& Context::deviceContext() const noexcept {
    return m_deviceContext;
}

VkInstance Context::instance() const noexcept {
    return m_instance;
}

VkSurfaceKHR Context::surface() const noexcept {
    return m_surface;
}

const PhysicalDeviceInfo& Context::physicalDeviceInfo() const noexcept {
    return m_physicalDeviceInfo;
}

void Context::destroy() noexcept {
    if (m_deviceContext.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_deviceContext.device);
    }
    if (m_deviceContext.allocator != VK_NULL_HANDLE) {
        vmaDestroyAllocator(m_deviceContext.allocator);
    }
    if (m_deviceContext.device != VK_NULL_HANDLE) {
        vkDestroyDevice(m_deviceContext.device, nullptr);
    }
    if (m_instance != VK_NULL_HANDLE && m_surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    }

    m_deviceContext = {};
    m_physicalDeviceInfo = {};
    m_surface = VK_NULL_HANDLE;
    m_debugUtils = {};

    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
    }
    m_instance = VK_NULL_HANDLE;
    m_validationEnabled = false;
}

} // namespace harmonia
