#include "harmonia/renderer/Pipeline.hpp"

#include <volk/volk.h>

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <utility>
#include <vector>

#include "harmonia/core/ShaderModule.hpp"
#include "harmonia/renderer/Descriptors.hpp"

namespace harmonia {

std::expected<Pipeline, VkResult> Pipeline::create(const DeviceContext& ctx,
                                                   const Descriptors& descriptors,
                                                   const ShaderPaths& paths,
                                                   std::uint32_t maxRayRecursion) {
    std::array<VkShaderModule, 7> modules{};
    const std::array shaderPaths{
        paths.raygen,
        paths.closesthitTriangle,
        paths.closesthitSphere,
        paths.intersection,
        paths.miss,
        paths.shadowMiss,
        paths.shadowAnyHit,
    };

    for (std::size_t i = 0; i < shaderPaths.size(); ++i) {
        auto module = harmonia::createShaderModule(ctx.device, shaderPaths[i]);
        if (!module) {
            for (VkShaderModule created : modules) {
                if (created != VK_NULL_HANDLE) {
                    vkDestroyShaderModule(ctx.device, created, nullptr);
                }
            }
            return std::unexpected(module.error());
        }
        modules[i] = *module;
    }

    // Stage list (index == position in rtStages below). The shadow any-hit module
    // supplies two entry points, so it appears twice as a stage while being one
    // VkShaderModule.
    const std::array rtEntryPoints{
        "main",                 // 0 raygen
        "triangleClosestHit",   // 1
        "sphereClosestHit",     // 2
        "main",                 // 3 intersection
        "main",                 // 4 miss
        "main",                 // 5 shadow miss
        "triangleShadowAnyHit", // 6
        "sphereShadowAnyHit",   // 7
    };
    const std::array rtStages{
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                        nullptr,
                                        0,
                                        VK_SHADER_STAGE_RAYGEN_BIT_KHR,
                                        modules[0],
                                        rtEntryPoints[0],
                                        nullptr},
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                        nullptr,
                                        0,
                                        VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
                                        modules[1],
                                        rtEntryPoints[1],
                                        nullptr},
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                        nullptr,
                                        0,
                                        VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
                                        modules[2],
                                        rtEntryPoints[2],
                                        nullptr},
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                        nullptr,
                                        0,
                                        VK_SHADER_STAGE_INTERSECTION_BIT_KHR,
                                        modules[3],
                                        rtEntryPoints[3],
                                        nullptr},
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                        nullptr,
                                        0,
                                        VK_SHADER_STAGE_MISS_BIT_KHR,
                                        modules[4],
                                        rtEntryPoints[4],
                                        nullptr},
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                        nullptr,
                                        0,
                                        VK_SHADER_STAGE_MISS_BIT_KHR,
                                        modules[5],
                                        rtEntryPoints[5],
                                        nullptr},
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                        nullptr,
                                        0,
                                        VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
                                        modules[6],
                                        rtEntryPoints[6],
                                        nullptr},
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                        nullptr,
                                        0,
                                        VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
                                        modules[6],
                                        rtEntryPoints[7],
                                        nullptr},
    };
    // Groups: [0] raygen, [1] miss, [2] shadow miss, then the four hit groups in
    // SBT order — triangle/radiance, triangle/shadow, procedural/radiance,
    // procedural/shadow (see Pipeline::kHitGroupCount). Radiance hit groups carry
    // no any-hit: their opacity is the estimator's stochastic pass-through gate,
    // so traversal must hand them every candidate. Shadow hit groups carry ONLY
    // the any-hit, which accumulates the OpenPBR (1-α) transmittance.
    const std::array groups{
        VkRayTracingShaderGroupCreateInfoKHR{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
                                             nullptr,
                                             VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
                                             0,
                                             VK_SHADER_UNUSED_KHR,
                                             VK_SHADER_UNUSED_KHR,
                                             VK_SHADER_UNUSED_KHR,
                                             nullptr},
        VkRayTracingShaderGroupCreateInfoKHR{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
                                             nullptr,
                                             VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
                                             4,
                                             VK_SHADER_UNUSED_KHR,
                                             VK_SHADER_UNUSED_KHR,
                                             VK_SHADER_UNUSED_KHR,
                                             nullptr},
        VkRayTracingShaderGroupCreateInfoKHR{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
                                             nullptr,
                                             VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
                                             5,
                                             VK_SHADER_UNUSED_KHR,
                                             VK_SHADER_UNUSED_KHR,
                                             VK_SHADER_UNUSED_KHR,
                                             nullptr},
        VkRayTracingShaderGroupCreateInfoKHR{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
                                             nullptr,
                                             VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
                                             VK_SHADER_UNUSED_KHR,
                                             1,
                                             VK_SHADER_UNUSED_KHR,
                                             VK_SHADER_UNUSED_KHR,
                                             nullptr},
        VkRayTracingShaderGroupCreateInfoKHR{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
                                             nullptr,
                                             VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
                                             VK_SHADER_UNUSED_KHR,
                                             VK_SHADER_UNUSED_KHR,
                                             6,
                                             VK_SHADER_UNUSED_KHR,
                                             nullptr},
        VkRayTracingShaderGroupCreateInfoKHR{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
                                             nullptr,
                                             VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR,
                                             VK_SHADER_UNUSED_KHR,
                                             2,
                                             VK_SHADER_UNUSED_KHR,
                                             3,
                                             nullptr},
        VkRayTracingShaderGroupCreateInfoKHR{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
                                             nullptr,
                                             VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR,
                                             VK_SHADER_UNUSED_KHR,
                                             VK_SHADER_UNUSED_KHR,
                                             7,
                                             3,
                                             nullptr},
    };
    // When the device supports opacity micromaps, mark the RT pipeline as
    // OMM-capable so it may trace against BLAS geometry that carries one. The
    // flag is harmless when a scene uses no OMM; it only enables traversal of
    // micromapped geometry when present.
    const VkPipelineCreateFlags pipelineFlags =
        ctx.opacityMicromapSupported ? VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_EXT : 0;
    const VkRayTracingPipelineCreateInfoKHR rtPipelineInfo{
        .sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
        .pNext = nullptr,
        .flags = pipelineFlags,
        .stageCount = static_cast<std::uint32_t>(rtStages.size()),
        .pStages = rtStages.data(),
        .groupCount = static_cast<std::uint32_t>(groups.size()),
        .pGroups = groups.data(),
        .maxPipelineRayRecursionDepth = maxRayRecursion,
        .pLibraryInfo = nullptr,
        .pLibraryInterface = nullptr,
        .pDynamicState = nullptr,
        .layout = descriptors.pipelineLayout(),
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = 0,
    };

    VkPipeline rtPipeline{VK_NULL_HANDLE};
    if (const VkResult result = vkCreateRayTracingPipelinesKHR(
            ctx.device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &rtPipelineInfo, nullptr, &rtPipeline);
        result != VK_SUCCESS) {
        for (VkShaderModule module : modules) {
            vkDestroyShaderModule(ctx.device, module, nullptr);
        }
        return std::unexpected(result);
    }

    ctx.setDebugName(VK_OBJECT_TYPE_PIPELINE, rtPipeline, "hyperion.rtPipeline");

    for (VkShaderModule module : modules) {
        vkDestroyShaderModule(ctx.device, module, nullptr);
    }

    Pipeline pipeline;
    pipeline.m_rtPipeline = harmonia::UniquePipeline{ctx.device, rtPipeline};
    return pipeline;
}

} // namespace harmonia
