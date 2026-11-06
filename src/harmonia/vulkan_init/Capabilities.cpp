#include "harmonia/vulkan_init/Capabilities.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace harmonia::vulkan_init {

[[nodiscard]] SupportedFeatures querySupportedFeatures(VkPhysicalDevice device) {
    SupportedFeatures s{};
    s.addressCommands.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_ADDRESS_COMMANDS_FEATURES_KHR;
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
    s.omm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_KHR;
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
    s.presentIdV1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
    s.memoryPriority.pNext = &s.presentIdV1;
    s.presentWaitV1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR;
    s.presentIdV1.pNext = &s.presentWaitV1;
    // MOD6: v2 present features (extend v1, not replace — both queried and enabled).
    s.presentIdV2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR;
    s.presentWaitV2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR;
    s.presentIdV2.pNext = &s.presentWaitV2;
    s.presentWaitV1.pNext = &s.presentIdV2;
    s.fifoLatestReady.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_MODE_FIFO_LATEST_READY_FEATURES_KHR;
    s.presentWaitV2.pNext = &s.fifoLatestReady;
    // VK16: device-lost forensics — the fault features ride the supported chain
    // between the present trio and the MOD5/VK14 tail (addressCommands …).
    s.fault.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
    s.addressBindingReport.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ADDRESS_BINDING_REPORT_FEATURES_EXT;
    s.fault.pNext = &s.addressBindingReport;
    s.addressBindingReport.pNext = &s.addressCommands;
    // MOD5: the full chain from features2 down through the present features ends at
    // fifoLatestReady; the VK16 fault nodes extend it, addressCommands follows.
    s.fifoLatestReady.pNext = &s.fault;
    // VK14: robustness2 features (nullDescriptor queried at the tail).
    s.robustness2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_KHR;
    // MOD1: descriptor buffer features.
    s.descriptorBuffer.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT;
    s.robustness2.pNext = &s.descriptorBuffer;
    s.addressCommands.pNext = &s.robustness2;
    // VK18 (2026 revision) + VK4 subgroup set — probed at the supported-chain tail.
    // NOTE: shader_subgroup_rotate is NOT queried standalone — it was promoted into
    // VkPhysicalDeviceVulkan14Features (read from s.features14 below); chaining the
    // standalone KHR struct alongside Vulkan14Features trips VUID-pNext-10360.
    s.descriptorBuffer.pNext = &s.swapchainMaintenance1;
    s.swapchainMaintenance1.pNext = &s.presentTiming;
    s.presentTiming.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT;
    s.presentTiming.pNext = &s.maximalReconvergence;
    s.maximalReconvergence.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MAXIMAL_RECONVERGENCE_FEATURES_KHR;
    s.maximalReconvergence.pNext = &s.subgroupUniformControlFlow;
    s.subgroupUniformControlFlow.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_UNIFORM_CONTROL_FLOW_FEATURES_KHR;
    s.subgroupUniformControlFlow.pNext = &s.quadControl;
    s.quadControl.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_QUAD_CONTROL_FEATURES_KHR;
    vkGetPhysicalDeviceFeatures2(device, &s.features2);
    return s;
}

// Optional capability flags, staged by the probe table below and copied into
// DeviceContext after a successful vkCreateDevice (the DeviceContext runtime
// branching API itself stays unchanged). Mesh shaders deliberately have no
// DeviceContext flag: registration must work on GPUs without VK_EXT_mesh_shader.

// The declarative probe table (replaces the former hand-written probe→flag
// blocks). Row order mirrors the former extension-push sequence.
[[nodiscard]] std::vector<CapabilityProbe> capabilityProbes(const PhysicalDeviceInfo& info,
                                                            const SupportedFeatures& supported) {
    return {
        CapabilityProbe{{VK_EXT_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME, nullptr},
                        &supported.ser.rayTracingInvocationReorder,
                        nullptr,
                        &CapabilityFlags::serSupported},
        CapabilityProbe{{VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME, nullptr},
                        &supported.positionFetch.rayTracingPositionFetch,
                        nullptr,
                        &CapabilityFlags::positionFetchSupported},
        CapabilityProbe{{VK_EXT_MESH_SHADER_EXTENSION_NAME, nullptr},
                        &supported.mesh.meshShader,
                        nullptr,
                        &CapabilityFlags::meshShaderSupported},
        CapabilityProbe{{VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME, nullptr},
                        &supported.dgc.deviceGeneratedCommands,
                        &info.dgcSupported,
                        &CapabilityFlags::dgcSupported},
        CapabilityProbe{{VK_KHR_OPACITY_MICROMAP_EXTENSION_NAME, nullptr},
                        &supported.omm.micromap,
                        &info.opacityMicromapSupported,
                        &CapabilityFlags::opacityMicromapSupported},
        CapabilityProbe{{VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME, VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME},
                        nullptr,
                        &info.pageableMemorySupported,
                        &CapabilityFlags::pageableMemorySupported},
        CapabilityProbe{{VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME, nullptr},
                        nullptr,
                        &info.calibratedTimestampsSupported,
                        &CapabilityFlags::calibratedTimestampsSupported},
        // MOD6: present_id2 extends (not replaces) present_id — both must be enabled.
        CapabilityProbe{{VK_KHR_PRESENT_ID_EXTENSION_NAME, VK_KHR_PRESENT_ID_2_EXTENSION_NAME},
                        &supported.presentIdV1.presentId,
                        &info.presentIdSupported,
                        &CapabilityFlags::presentIdSupported},
        // MOD6: present_wait2 extends (not replaces) present_wait — both must be enabled.
        CapabilityProbe{{VK_KHR_PRESENT_WAIT_EXTENSION_NAME, VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME},
                        &supported.presentWaitV1.presentWait,
                        &info.presentWaitSupported,
                        &CapabilityFlags::presentWaitSupported},
        CapabilityProbe{{VK_KHR_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME, nullptr},
                        &supported.fifoLatestReady.presentModeFifoLatestReady,
                        &info.fifoLatestReadySupported,
                        &CapabilityFlags::fifoLatestReadySupported},
        // VK16: device-lost forensics — both EXT, cross-vendor. Probe the deviceFault
        // feature bit; deviceFaultVendorBinary is enabled per-feature at enable time
        // (the raw vendor blob dump needs it).
        CapabilityProbe{{VK_EXT_DEVICE_FAULT_EXTENSION_NAME, nullptr},
                        &supported.fault.deviceFault,
                        &info.deviceFaultSupported,
                        &CapabilityFlags::deviceFaultSupported},
        CapabilityProbe{{VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME, nullptr},
                        &supported.addressBindingReport.reportAddressBinding,
                        &info.addressBindingReportSupported,
                        &CapabilityFlags::addressBindingReportSupported},
        // MOD4 v1: swapchain_maintenance1 — the fence/release half (per-present fences
        // + vkReleaseSwapchainImagesKHR on resize; see Swapchain::recreate). The KHR
        // name is the promoted extension (the VK_EXT_swapchain_maintenance1 alias is
        // legacy). The present-scaling sub-feature stays out: the probe found
        // supportedPresentScaling = 0 for every present mode on the reference GPU
        // (RTX 5070 / driver 621.01). Extension-only fallback: devices without the
        // extension keep the legacy device-idle resize path (Swapchain::recreate).
        CapabilityProbe{{VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME, nullptr},
                        &supported.swapchainMaintenance1.swapchainMaintenance1,
                        &info.swapchainMaintenance1Supported,
                        &CapabilityFlags::swapchainMaintenance1Supported},
        // VK21: VRAM budget queries — extension-only probe (no feature struct; the budget
        // properties ride the properties2 chain once the extension is enabled).
        CapabilityProbe{{VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, nullptr},
                        nullptr,
                        &info.memoryBudgetSupported,
                        &CapabilityFlags::memoryBudgetSupported},
        // VK18 (2026 revision): per-present timing records + past-presentation queries.
        // Both gates: the device feature bit AND the enumerate-time extension flag.
        CapabilityProbe{{VK_EXT_PRESENT_TIMING_EXTENSION_NAME, nullptr},
                        &supported.presentTiming.presentTiming,
                        &info.presentTimingSupported,
                        &CapabilityFlags::presentTimingSupported},
        // VK4 subgroup capability set (GI/PERF enabler: warp-coherent reductions/rotations
        // for ReSTIR reservoir merging & compaction; converged-wavefront guarantees for the
        // path integrator). Feature-only probes; each extension enables independently so
        // partial devices simply do not use the missing pieces.
        CapabilityProbe{{VK_KHR_SHADER_SUBGROUP_ROTATE_EXTENSION_NAME, nullptr},
                        &supported.features14.shaderSubgroupRotate,
                        nullptr,
                        &CapabilityFlags::subgroupRotateSupported},
        CapabilityProbe{{VK_KHR_SHADER_MAXIMAL_RECONVERGENCE_EXTENSION_NAME, nullptr},
                        &supported.maximalReconvergence.shaderMaximalReconvergence,
                        nullptr,
                        &CapabilityFlags::shaderMaximalReconvergenceSupported},
        CapabilityProbe{{VK_KHR_SHADER_SUBGROUP_UNIFORM_CONTROL_FLOW_EXTENSION_NAME, nullptr},
                        &supported.subgroupUniformControlFlow.shaderSubgroupUniformControlFlow,
                        nullptr,
                        &CapabilityFlags::shaderSubgroupUniformControlFlowSupported},
        CapabilityProbe{{VK_KHR_SHADER_QUAD_CONTROL_EXTENSION_NAME, nullptr},
                        &supported.quadControl.shaderQuadControl,
                        nullptr,
                        &CapabilityFlags::shaderQuadControlSupported},
    };
}

// Fills @p features (caller-owned so the pNext chain survives vkCreateDevice) and
// returns the head pointer to thread into VkDeviceCreateInfo::pNext.
[[nodiscard]] const void* buildEnabledFeatures(EnabledFeatures& features,
                                               const SupportedFeatures& supported,
                                               const CapabilityFlags& capabilities,
                                               const std::vector<std::string>& maintenanceExtensions) {
    features.rayQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    features.rayQuery.rayQuery = VK_TRUE;

    features.rt.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    features.rt.pNext = &features.omm;
    features.rt.rayTracingPipeline = VK_TRUE;

    // VK_KHR_opacity_micromap: per-microtriangle opacity for alpha-tested
    // geometry, resolved by RT traversal (no any-hit shader). Optional; enabled
    // only when the device advertises it. Sits between rt and rayQuery in the
    // always-on core so the optional present/pageable tail still hangs off
    // rayQuery unchanged.
    features.omm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_KHR;
    features.omm.pNext = &features.rayQuery;
    features.omm.micromap = capabilities.opacityMicromapSupported ? VK_TRUE : VK_FALSE;

    features.as.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    features.as.pNext = &features.rt;
    features.as.accelerationStructure = VK_TRUE;
    features.as.descriptorBindingAccelerationStructureUpdateAfterBind = VK_TRUE;

    features.ser.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_EXT;
    features.ser.pNext = &features.as;
    features.ser.rayTracingInvocationReorder = capabilities.serSupported ? VK_TRUE : VK_FALSE;

    features.rtMaintenance1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MAINTENANCE_1_FEATURES_KHR;
    features.rtMaintenance1.pNext = &features.ser;
    features.rtMaintenance1.rayTracingMaintenance1 = VK_TRUE;
    features.rtMaintenance1.rayTracingPipelineTraceRaysIndirect2 = VK_TRUE;

    features.positionFetch.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR;
    features.positionFetch.pNext = &features.rtMaintenance1;
    features.positionFetch.rayTracingPositionFetch = capabilities.positionFetchSupported ? VK_TRUE : VK_FALSE;

    features.features14.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
    features.features14.pushDescriptor = VK_TRUE;
    features.features14.maintenance5 = VK_TRUE;
    features.features14.hostImageCopy = VK_TRUE;
    // VK4: shader_subgroup_rotate was promoted into Vulkan14Features — the standalone
    // KHR struct must NOT appear in the device chain alongside it (VUID-pNext-10360).
    features.features14.shaderSubgroupRotate = capabilities.subgroupRotateSupported ? VK_TRUE : VK_FALSE;
    features.features14.shaderSubgroupRotateClustered =
        (capabilities.subgroupRotateSupported && supported.features14.shaderSubgroupRotateClustered == VK_TRUE)
            ? VK_TRUE
            : VK_FALSE;

    // MOD5: device-address-only AS creation — hard-required (the legacy
    // vkCreateAccelerationStructureKHR is dropped; no fallback). Inserted in the
    // always-present main chain between features14 and positionFetch.
    features.addressCommands.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_ADDRESS_COMMANDS_FEATURES_KHR;
    features.addressCommands.deviceAddressCommands = VK_TRUE;

    // VK14: robustness2 — enable nullDescriptor only (eliminates dummy resources).
    // robustBufferAccess2/ImageAccess2 are deliberately NOT enabled: they require the
    // core robustBufferAccess feature which has a performance cost; nullDescriptor alone
    // provides the binding-cleanup benefit we need.
    features.robustness2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_KHR;
    features.robustness2.nullDescriptor = VK_TRUE;
    features.robustness2.robustBufferAccess2 = VK_FALSE;
    features.robustness2.robustImageAccess2 = VK_FALSE;
    // MOD1: descriptor buffer — the modern GPU-driven descriptor path.
    features.descriptorBuffer.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT;
    features.descriptorBuffer.descriptorBuffer = VK_TRUE;
    features.descriptorBuffer.descriptorBufferCaptureReplay = VK_TRUE;
    features.robustness2.pNext = &features.descriptorBuffer;
    features.addressCommands.pNext = &features.robustness2;

    // Auto-enabled KHR maintenance extensions (see createDevice): additive,
    // cross-vendor semantics, enabled iff advertised — no runtime branch. The
    // SDK declares feature structs for maintenance7..11, so chain and enable
    // them like every other per-extension feature. maintenance4/5 ride the core
    // feature structs above; maintenance6's feature bit stays off (its struct is
    // deliberately not chained — the extension string is still auto-enabled).
    const auto enablesMaintenance = [&maintenanceExtensions](const char* name) {
        return std::any_of(maintenanceExtensions.begin(),
                           maintenanceExtensions.end(),
                           [name](const std::string& extension) { return extension == name; });
    };
    const bool maintenance7Enabled = enablesMaintenance(VK_KHR_MAINTENANCE_7_EXTENSION_NAME);
    const bool maintenance8Enabled = enablesMaintenance(VK_KHR_MAINTENANCE_8_EXTENSION_NAME);
    const bool maintenance9Enabled = enablesMaintenance(VK_KHR_MAINTENANCE_9_EXTENSION_NAME);
    const bool maintenance10Enabled = enablesMaintenance(VK_KHR_MAINTENANCE_10_EXTENSION_NAME);
    const bool maintenance11Enabled = enablesMaintenance(VK_KHR_MAINTENANCE_11_EXTENSION_NAME);
    features.maintenance7.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_7_FEATURES_KHR;
    features.maintenance7.maintenance7 = VK_TRUE;
    features.maintenance8.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_8_FEATURES_KHR;
    features.maintenance8.maintenance8 = VK_TRUE;
    features.maintenance9.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_9_FEATURES_KHR;
    features.maintenance9.maintenance9 = VK_TRUE;
    features.maintenance10.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_10_FEATURES_KHR;
    features.maintenance10.maintenance10 = VK_TRUE;
    features.maintenance11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_11_FEATURES_KHR;
    features.maintenance11.maintenance11 = VK_TRUE;
    // Only an enabled extension may contribute a feature struct to the chain
    // (same convention as the optional present trio below): splice the enabled
    // maintenance nodes right-to-left between descriptorBuffer and positionFetch.
    void* maintenanceNext = &features.positionFetch;
    if (maintenance11Enabled) {
        features.maintenance11.pNext = maintenanceNext;
        maintenanceNext = &features.maintenance11;
    }
    if (maintenance10Enabled) {
        features.maintenance10.pNext = maintenanceNext;
        maintenanceNext = &features.maintenance10;
    }
    if (maintenance9Enabled) {
        features.maintenance9.pNext = maintenanceNext;
        maintenanceNext = &features.maintenance9;
    }
    if (maintenance8Enabled) {
        features.maintenance8.pNext = maintenanceNext;
        maintenanceNext = &features.maintenance8;
    }
    if (maintenance7Enabled) {
        features.maintenance7.pNext = maintenanceNext;
        maintenanceNext = &features.maintenance7;
    }
    // MOD4 v1: swapchain_maintenance1 — the fence/release half (per-present fences +
    // vkReleaseSwapchainImagesKHR on resize). NOT the present-scaling sub-feature:
    // VkSwapchainPresentScalingCreateInfoKHR was probed (supportedPresentScaling = 0
    // on the reference GPU) and stays out. Linked only when the probe enabled the
    // extension (convention: only an enabled extension contributes a feature struct).
    if (capabilities.swapchainMaintenance1Supported) {
        features.swapchainMaintenance1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR;
        features.swapchainMaintenance1.swapchainMaintenance1 = VK_TRUE;
        features.swapchainMaintenance1.pNext = maintenanceNext;
        maintenanceNext = &features.swapchainMaintenance1;
    }
    features.descriptorBuffer.pNext = maintenanceNext;

    // The always-present main chain head: features14 → addressCommands (MOD5,
    // hard-required) → the VK14/MOD1 optional tail above.
    features.features14.pNext = &features.addressCommands;

    features.features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features.features13.pNext = &features.features14;
    features.features13.synchronization2 = VK_TRUE;
    features.features13.dynamicRendering = VK_TRUE;
    features.features13.maintenance4 = VK_TRUE;
    // Hard-required: the geometry_opacity cutout path (C14) uses Slang `discard` in
    // forward_render.frag.slang, so the emitted SPIR-V declares the DemoteToHelperInvocation
    // capability — the device feature must be enabled (VUID-08740).
    features.features13.shaderDemoteToHelperInvocation = VK_TRUE;

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

    // scene::Mesh/task shaders are optional: required by Theia's rasterizer, unused by Hyperion's
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
        capabilities.meshShaderSupported ? static_cast<void*>(&features.mesh) : static_cast<void*>(&features.features2);
    features.dgc.deviceGeneratedCommands = VK_TRUE;

    // VK_EXT_pageable_device_local_memory (depends on VK_EXT_memory_priority): lets the driver
    // page device-local memory by priority. The two extensions + the memoryPriority feature are
    // enabled together so VMA can assign per-allocation priorities (VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT on the
    // allocator).
    features.memoryPriority.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT;
    features.memoryPriority.memoryPriority = capabilities.pageableMemorySupported ? VK_TRUE : VK_FALSE;

    // Present-pacing trio (present_id / present_wait / present_mode_fifo_latest_ready): each is
    // optional; only link a feature struct into the pNext chain when its extension is enabled.
    features.presentIdV1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
    features.presentIdV1.presentId = capabilities.presentIdSupported ? VK_TRUE : VK_FALSE;
    features.presentWaitV1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR;
    features.presentWaitV1.presentWait = capabilities.presentWaitSupported ? VK_TRUE : VK_FALSE;
    // MOD6: v2 features enabled alongside v1 (v2 extends v1, not replaces).
    features.presentIdV2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR;
    features.presentIdV2.presentId2 = capabilities.presentIdSupported ? VK_TRUE : VK_FALSE;
    features.presentWaitV2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR;
    features.presentWaitV2.presentWait2 = capabilities.presentWaitSupported ? VK_TRUE : VK_FALSE;
    features.fifoLatestReady.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_MODE_FIFO_LATEST_READY_FEATURES_KHR;
    features.fifoLatestReady.presentModeFifoLatestReady = capabilities.fifoLatestReadySupported ? VK_TRUE : VK_FALSE;

    // VK16: device-lost forensics (VK_EXT_device_fault + VK_EXT_device_address_binding_report)
    // ride the optional tail after the present trio. Conservative enable: `deviceFault` is
    // what the retrieval path needs; `deviceFaultVendorBinary` additionally gates the raw
    // vendor blob dump. Only an enabled extension contributes its feature struct (convention).
    features.fault.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
    features.fault.deviceFault = capabilities.deviceFaultSupported ? VK_TRUE : VK_FALSE;
    features.fault.deviceFaultVendorBinary =
        (capabilities.deviceFaultSupported && supported.fault.deviceFaultVendorBinary == VK_TRUE) ? VK_TRUE : VK_FALSE;
    features.addressBindingReport.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ADDRESS_BINDING_REPORT_FEATURES_EXT;
    features.addressBindingReport.reportAddressBinding =
        capabilities.addressBindingReportSupported ? VK_TRUE : VK_FALSE;
    // Terminal target of the optional tail: the enabled VK16 nodes (address binding report
    // after fault), or nullptr when neither extension is enabled.
    void* vk16Tail = nullptr;
    if (capabilities.deviceFaultSupported) {
        features.fault.pNext = capabilities.addressBindingReportSupported ? &features.addressBindingReport : nullptr;
        vk16Tail = &features.fault;
    } else if (capabilities.addressBindingReportSupported) {
        features.addressBindingReport.pNext = nullptr;
        vk16Tail = &features.addressBindingReport;
    }

    // VK18 + VK4: pipeline/presentation capability set, linked in front of the VK16
    // nodes. Only an enabled extension contributes its feature struct (convention).
    void* capabilityTail = vk16Tail;
    // (rotate rides Vulkan14Features — promoted, no standalone node allowed next to it.)
    if (capabilities.shaderMaximalReconvergenceSupported) {
        features.maximalReconvergence.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MAXIMAL_RECONVERGENCE_FEATURES_KHR;
        features.maximalReconvergence.shaderMaximalReconvergence = VK_TRUE;
        features.maximalReconvergence.pNext = capabilityTail;
        capabilityTail = &features.maximalReconvergence;
    }
    if (capabilities.shaderSubgroupUniformControlFlowSupported) {
        features.subgroupUniformControlFlow.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_UNIFORM_CONTROL_FLOW_FEATURES_KHR;
        features.subgroupUniformControlFlow.shaderSubgroupUniformControlFlow = VK_TRUE;
        features.subgroupUniformControlFlow.pNext = capabilityTail;
        capabilityTail = &features.subgroupUniformControlFlow;
    }
    if (capabilities.shaderQuadControlSupported) {
        features.quadControl.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_QUAD_CONTROL_FEATURES_KHR;
        features.quadControl.shaderQuadControl = VK_TRUE;
        features.quadControl.pNext = capabilityTail;
        capabilityTail = &features.quadControl;
    }
    if (capabilities.presentTimingSupported) {
        features.presentTiming.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT;
        features.presentTiming.presentTiming = VK_TRUE;
        // presentAtAbsoluteTime/RelativeTime stay off: this adoption is query-only
        // (vkGetPastPresentationTimingEXT); scheduling presents at a target time is
        // a later I-track step.
        features.presentTiming.pNext = capabilityTail;
        capabilityTail = &features.presentTiming;
    }

    // Link the optional tail chain in order (memoryPriority → presentId → presentWait → fifoLatestReady
    // → [VK18/VK4 nodes] → [VK16 fault nodes]); each enabled node points to the next enabled node,
    // rayQuery heads it.
    features.fifoLatestReady.pNext = capabilityTail;
    features.presentWaitV1.pNext =
        capabilities.fifoLatestReadySupported ? static_cast<void*>(&features.fifoLatestReady) : capabilityTail;
    features.presentIdV1.pNext =
        capabilities.presentWaitSupported
            ? static_cast<void*>(&features.presentWaitV1)
            : (capabilities.fifoLatestReadySupported ? static_cast<void*>(&features.fifoLatestReady) : capabilityTail);
    features.memoryPriority.pNext =
        capabilities.presentIdSupported
            ? static_cast<void*>(&features.presentIdV1)
            : (capabilities.presentWaitSupported
                   ? static_cast<void*>(&features.presentWaitV1)
                   : (capabilities.fifoLatestReadySupported ? static_cast<void*>(&features.fifoLatestReady)
                                                            : capabilityTail));
    features.rayQuery.pNext =
        capabilities.pageableMemorySupported
            ? static_cast<void*>(&features.memoryPriority)
            : (capabilities.presentIdSupported
                   ? static_cast<void*>(&features.presentIdV1)
                   : (capabilities.presentWaitSupported
                          ? static_cast<void*>(&features.presentWaitV1)
                          : (capabilities.fifoLatestReadySupported ? static_cast<void*>(&features.fifoLatestReady)
                                                                   : capabilityTail)));

    // MOD6: insert v2 present features right after their v1 counterparts (v2 extends v1,
    // both are always enabled together). The insertion takes over v1's pNext and chains
    // through v2 back to the original target — no conditional re-linking needed.
    if (capabilities.presentIdSupported) {
        features.presentIdV2.pNext = features.presentIdV1.pNext;
        features.presentIdV1.pNext = &features.presentIdV2;
    }
    if (capabilities.presentWaitSupported) {
        features.presentWaitV2.pNext = features.presentWaitV1.pNext;
        features.presentWaitV1.pNext = &features.presentWaitV2;
    }

    return capabilities.dgcSupported          ? static_cast<const void*>(&features.dgc)
           : capabilities.meshShaderSupported ? static_cast<const void*>(&features.mesh)
                                              : static_cast<const void*>(&features.features2);
}

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

} // namespace harmonia::vulkan_init
