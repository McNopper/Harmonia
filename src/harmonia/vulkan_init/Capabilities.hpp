#ifndef HARMONIA_VULKAN_INIT_CAPABILITIES_HPP
#define HARMONIA_VULKAN_INIT_CAPABILITIES_HPP

#include <volk/volk.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "harmonia/vulkan_init/PhysicalDevice.hpp"

namespace harmonia::vulkan_init {

// Device capability surface (extracted from Context.cpp - the file outgrew its
// architecture-gate size limit): the supported-feature chain, the declarative
// capability probe table, and the enabled-feature chain construction.

struct SupportedFeatures {
    VkPhysicalDeviceRayQueryFeaturesKHR rayQuery{};
    VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR rtMaintenance1{};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt{};
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh{};
    VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT ser{};
    VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR positionFetch{};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as{};
    VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT dgc{};
    VkPhysicalDeviceOpacityMicromapFeaturesKHR omm{};
    VkPhysicalDeviceMemoryPriorityFeaturesEXT memoryPriority{};
    VkPhysicalDevicePresentIdFeaturesKHR presentIdV1{};
    VkPhysicalDevicePresentWaitFeaturesKHR presentWaitV1{};
    VkPhysicalDevicePresentId2FeaturesKHR presentIdV2{};
    VkPhysicalDevicePresentWait2FeaturesKHR presentWaitV2{};
    VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR fifoLatestReady{};
    VkPhysicalDeviceFaultFeaturesEXT fault{};
    VkPhysicalDeviceAddressBindingReportFeaturesEXT addressBindingReport{};
    VkPhysicalDeviceVulkan14Features features14{};
    VkPhysicalDeviceVulkan13Features features13{};
    VkPhysicalDeviceVulkan12Features features12{};
    VkPhysicalDeviceVulkan11Features features11{};
    VkPhysicalDeviceFeatures2 features2{};
    VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR addressCommands{};
    VkPhysicalDeviceRobustness2FeaturesKHR robustness2{};
    VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptorBuffer{};
    VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR swapchainMaintenance1{};
    // VK18 (2026 revision) + VK4 subgroup set — probed at the supported-chain tail.
    // (shader_subgroup_rotate reads from Vulkan14Features — promoted, see the chain note.)
    VkPhysicalDevicePresentTimingFeaturesEXT presentTiming{};
    VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR maximalReconvergence{};
    VkPhysicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR subgroupUniformControlFlow{};
    VkPhysicalDeviceShaderQuadControlFeaturesKHR quadControl{};
    // VK5 (re-investigation): explicit pipeline-binary capture/consume cache.
    VkPhysicalDevicePipelineBinaryFeaturesKHR pipelineBinary{};
};

[[nodiscard]] SupportedFeatures querySupportedFeatures(VkPhysicalDevice device);
struct CapabilityFlags {
    bool serSupported = false;
    bool positionFetchSupported = false;
    bool meshShaderSupported = false;
    bool dgcSupported = false;
    bool opacityMicromapSupported = false;
    bool pageableMemorySupported = false;
    bool calibratedTimestampsSupported = false;
    bool presentIdSupported = false;
    bool presentWaitSupported = false;
    bool fifoLatestReadySupported = false;
    bool deviceFaultSupported = false;
    bool addressBindingReportSupported = false;
    bool swapchainMaintenance1Supported = false;
    bool memoryBudgetSupported = false;                     ///< VK21
    bool presentTimingSupported = false;                    ///< VK18
    bool subgroupRotateSupported = false;                   ///< VK4
    bool shaderMaximalReconvergenceSupported = false;       ///< VK4
    bool shaderSubgroupUniformControlFlowSupported = false; ///< VK4
    bool shaderQuadControlSupported = false;                ///< VK4
    bool pipelineBinarySupported = false;                   ///< VK5
};

// One row per optional capability: on pass, the extension(s) are enabled and the
// capability flag is recorded. A row passes when the extension was advertised at
// enumerate time (infoFlag) AND — where the row carries one — the supported
// feature bit is TRUE (featureBit). Null slots opt out of a check, which is what
// distinguishes feature-only probes (ser, position fetch, mesh shader) from
// extension-only probes (pageable memory, calibrated timestamps). Feature fields
// are VkBool32 throughout and probed via type-erased pointers uniformly.
struct CapabilityProbe {
    std::array<const char*, 2> extensions; ///< Extension names enabled on pass; unused slots are null.
    const VkBool32* featureBit;            ///< Supported-chain feature bit; null = extension-only probe.
    const bool* infoFlag;                  ///< PhysicalDeviceInfo presence flag; null = feature-only probe.
    bool CapabilityFlags::* flag;          ///< Capability flag recorded on pass.
};

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
    VkPhysicalDeviceOpacityMicromapFeaturesKHR omm{};
    VkPhysicalDeviceMemoryPriorityFeaturesEXT memoryPriority{};
    VkPhysicalDevicePresentIdFeaturesKHR presentIdV1{};
    VkPhysicalDevicePresentWaitFeaturesKHR presentWaitV1{};
    VkPhysicalDevicePresentId2FeaturesKHR presentIdV2{};
    VkPhysicalDevicePresentWait2FeaturesKHR presentWaitV2{};
    VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR fifoLatestReady{};
    VkPhysicalDeviceFaultFeaturesEXT fault{};
    VkPhysicalDeviceAddressBindingReportFeaturesEXT addressBindingReport{};
    VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR addressCommands{};
    VkPhysicalDeviceRobustness2FeaturesKHR robustness2{};
    VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptorBuffer{};
    VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR swapchainMaintenance1{};
    // VK18 (2026 revision) + VK4 subgroup set (chained conditionally when enabled —
    // only an enabled extension contributes its feature struct). Rotate rides
    // Vulkan14Features (promoted — no standalone node allowed next to it).
    VkPhysicalDevicePresentTimingFeaturesEXT presentTiming{};
    VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR maximalReconvergence{};
    VkPhysicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR subgroupUniformControlFlow{};
    VkPhysicalDeviceShaderQuadControlFeaturesKHR quadControl{};
    // Auto-enabled KHR maintenance extensions with SDK-declared feature structs
    // (chained in buildEnabledFeatures when their extension was auto-enabled).
    VkPhysicalDeviceMaintenance7FeaturesKHR maintenance7{};
    VkPhysicalDeviceMaintenance8FeaturesKHR maintenance8{};
    VkPhysicalDeviceMaintenance9FeaturesKHR maintenance9{};
    VkPhysicalDeviceMaintenance10FeaturesKHR maintenance10{};
    VkPhysicalDeviceMaintenance11FeaturesKHR maintenance11{};
    // VK5 (re-investigation): explicit pipeline-binary capture/consume cache
    // (chained conditionally when enabled — only an enabled extension contributes
    // its feature struct).
    VkPhysicalDevicePipelineBinaryFeaturesKHR pipelineBinary{};
};

[[nodiscard]] const void* buildEnabledFeatures(EnabledFeatures& features,
                                               const SupportedFeatures& supported,
                                               const CapabilityFlags& capabilities,
                                               const std::vector<std::string>& maintenanceExtensions);

[[nodiscard]] std::vector<CapabilityProbe> capabilityProbes(const PhysicalDeviceInfo& info,
                                                            const SupportedFeatures& supported);

struct QueueInfos {
    std::vector<VkDeviceQueueCreateInfo> infos;
    std::uint32_t asyncComputeFamily = UINT32_MAX;
};

[[nodiscard]] QueueInfos buildQueueCreateInfos(VkPhysicalDevice device, std::uint32_t graphicsFamily);

} // namespace harmonia::vulkan_init
#endif // HARMONIA_VULKAN_INIT_CAPABILITIES_HPP
