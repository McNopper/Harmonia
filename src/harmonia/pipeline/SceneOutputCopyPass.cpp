#include "harmonia/pipeline/SceneOutputCopyPass.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <utility>
#include "harmonia/core/Barrier.hpp"
#include "harmonia/core/Logger.hpp"
#include "harmonia/core/Sampler.hpp"
#include "harmonia/core/ShaderModule.hpp"
#include "harmonia/pipeline/PassContext.hpp"
#include "harmonia/pipeline/SceneOutputCopyPassDetail.hpp"
namespace harmonia::pipeline {

std::expected<SceneOutputCopyPass, VkResult> SceneOutputCopyPass::create(const DeviceContext& ctx,
                                                                         VkExtent2D extent,
                                                                         const std::filesystem::path& computeSpvPath,
                                                                         const Settings& settings) {
    if (!ctx.isValid() || extent.width == 0U || extent.height == 0U) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    Settings s = settings;
    s.strength = clamp01(s.strength);
    s.historyBlend = clamp01(s.historyBlend);
    s.iterations = clampIterations(s.iterations);

    const std::array bindings{
        VkDescriptorSetLayoutBinding{
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        },
        VkDescriptorSetLayoutBinding{
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        },
        VkDescriptorSetLayoutBinding{
            .binding = 2,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        },
        VkDescriptorSetLayoutBinding{
            .binding = 3,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        },
        VkDescriptorSetLayoutBinding{
            .binding = 4,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        },
        VkDescriptorSetLayoutBinding{
            .binding = 5,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        },
        VkDescriptorSetLayoutBinding{
            .binding = 6,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        },
        VkDescriptorSetLayoutBinding{
            .binding = 7,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        },
    };
    const VkDescriptorSetLayoutCreateInfo setLayoutInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT,
        .bindingCount = static_cast<std::uint32_t>(bindings.size()),
        .pBindings = bindings.data(),
    };

    SceneOutputCopyPass pass;
    pass.m_ctx = &ctx;
    pass.m_extent = extent;
    pass.m_settings = settings;

    VkDescriptorSetLayout setLayout{};
    if (const VkResult result = vkCreateDescriptorSetLayout(ctx.device, &setLayoutInfo, nullptr, &setLayout);
        result != VK_SUCCESS) {
        return std::unexpected(result);
    }
    pass.m_setLayout = core::UniqueDescriptorSetLayout{ctx.device, setLayout};

    const VkPushConstantRange pushRange{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0U,
        .size = sizeof(DenoiserPushConstants),
    };
    const VkPipelineLayoutCreateInfo pipelineLayoutInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0U,
        .setLayoutCount = 1U,
        .pSetLayouts = pass.m_setLayout.ptr(),
        .pushConstantRangeCount = 1U,
        .pPushConstantRanges = &pushRange,
    };
    VkPipelineLayout pipelineLayout{};
    if (const VkResult result = vkCreatePipelineLayout(ctx.device, &pipelineLayoutInfo, nullptr, &pipelineLayout);
        result != VK_SUCCESS) {
        return std::unexpected(result);
    }
    pass.m_pipelineLayout = core::UniquePipelineLayout{ctx.device, pipelineLayout};

    const VkSamplerCreateInfo samplerInfo = core::makeSamplerCreateInfo({
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
    });
    VkSampler guideSampler{};
    if (const VkResult result = vkCreateSampler(ctx.device, &samplerInfo, nullptr, &guideSampler);
        result != VK_SUCCESS) {
        return std::unexpected(result);
    }
    pass.m_guideSampler = core::UniqueSampler{ctx.device, guideSampler};

    auto module = core::createShaderModule(ctx.device, computeSpvPath);
    if (!module) {
        return std::unexpected(module.error());
    }

    const VkPipelineShaderStageCreateInfo stage{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0U,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = *module,
        .pName = "main",
        .pSpecializationInfo = nullptr,
    };
    const VkComputePipelineCreateInfo pipelineInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0U,
        .stage = stage,
        .layout = pass.m_pipelineLayout,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = 0,
    };
    VkPipeline pipeline{};
    const VkResult pipelineResult =
        vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
    vkDestroyShaderModule(ctx.device, *module, nullptr);
    if (pipelineResult != VK_SUCCESS) {
        return std::unexpected(pipelineResult);
    }
    pass.m_pipeline = core::UniquePipeline{ctx.device, pipeline};

    if (!pass.createWorkImages(extent)) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    ctx.setDebugName(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, pass.m_setLayout.get(), "harmonia.denoiser.setLayout");
    ctx.setDebugName(VK_OBJECT_TYPE_PIPELINE_LAYOUT, pass.m_pipelineLayout.get(), "harmonia.denoiser.pipelineLayout");
    ctx.setDebugName(VK_OBJECT_TYPE_PIPELINE, pass.m_pipeline.get(), "harmonia.denoiser.pipeline");
    ctx.setDebugName(VK_OBJECT_TYPE_SAMPLER, pass.m_guideSampler.get(), "harmonia.denoiser.sampler");
    return pass;
}

void SceneOutputCopyPass::record(const PassContext& ctx) noexcept {
    if (m_ctx == nullptr || m_pipeline == VK_NULL_HANDLE || m_pipelineLayout == VK_NULL_HANDLE ||
        m_setLayout == VK_NULL_HANDLE || m_guideSampler == VK_NULL_HANDLE || ctx.cmd == VK_NULL_HANDLE ||
        ctx.hdrBuffer == nullptr || ctx.denoised == nullptr || !m_historyImage.isValid() || !m_workImage.isValid() ||
        !m_dummyMotionVectors.isValid()) {
        return;
    }
    if (ctx.extent.width == 0U || ctx.extent.height == 0U) {
        return;
    }
    if (ctx.extent.width != m_extent.width || ctx.extent.height != m_extent.height) {
        onResize(ctx.extent);
        if (!m_historyImage.isValid() || !m_workImage.isValid()) {
            return;
        }
    }

    if (ctx.denoiserResetToken != m_lastResetToken) {
        resetHistory(ctx.denoiserResetToken);
    }

    // A-SVGF gradient path is active only when enabled, the full-res gradient
    // buffers exist, and temporal history is actually applied (fixed view).
    const bool applyHistory = m_settings.useHistory && ctx.fixedView;
    const bool useGradient = m_settings.useGradient && m_gradientImage.isValid() && m_prevGradientImage.isValid();

    barrierForComputeRead(ctx.cmd, ctx.hdrBuffer->handle(), ctx.denoised->handle(), useGradient);

    // Resolve motion-vector binding: use the real view when available, fall back to
    // the 1×1 dummy (keeps the descriptor valid; hasMotionVectors=0 prevents reads).
    const bool hasMotionVectors = ctx.motionVectorView != VK_NULL_HANDLE;
    const VkImageView motionVecView = hasMotionVectors ? ctx.motionVectorView : m_dummyMotionVectors.view();

    // Gradient/variance descriptor views (bindings 6/7). Real images when the
    // gradient path is active; otherwise the shared 1×1 dummy.
    const VkImageView gradientView = useGradient ? m_gradientImage.view() : m_dummyGradient.view();
    const VkImageView prevGradientView = useGradient ? m_prevGradientImage.view() : m_dummyGradient.view();
    // The variance buffer only holds meaningful data once a prior temporal pass
    // has written it (i.e. gradient enabled, history applied, past first use).
    const bool hasGradientVariance = useGradient && applyHistory && !m_historyFirstUse;

    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    const std::uint32_t groupsX = dispatchGroups1D(ctx.extent.width);
    const std::uint32_t groupsY = dispatchGroups1D(ctx.extent.height);

    const VkImageView normalCandidate = ctx.gNormalView != VK_NULL_HANDLE ? ctx.gNormalView : ctx.transparentNormalView;
    const VkImageView depthCandidate = ctx.gDepthView != VK_NULL_HANDLE ? ctx.gDepthView : ctx.transparentDepthView;
    const bool hasNormalGuide = normalCandidate != VK_NULL_HANDLE;
    const bool hasDepthGuide = depthCandidate != VK_NULL_HANDLE;
    const VkImageView fallbackGuideView = ctx.hdrBuffer->view();
    const VkImageView normalGuideView = hasNormalGuide ? normalCandidate : fallbackGuideView;
    const VkImageView depthGuideView = hasDepthGuide ? depthCandidate : fallbackGuideView;

    const std::uint32_t iterations = clampIterations(m_settings.iterations);

    const DispatchGrid grid{
        .iterations = iterations,
        .groupsX = groupsX,
        .groupsY = groupsY,
    };
    const DenoiseGuides guides{
        .normalView = normalGuideView,
        .depthView = depthGuideView,
        .hasNormal = hasNormalGuide,
        .hasDepth = hasDepthGuide,
        .motionVecView = motionVecView,
        .gradientView = gradientView,
        .prevGradientView = prevGradientView,
        .hasGradientVariance = hasGradientVariance,
    };

    const VkImage finalSource = recordSpatialPasses(ctx.cmd, *ctx.hdrBuffer, *ctx.denoised, grid, guides);

    if (finalSource != ctx.denoised->handle()) {
        copyImageRoundTrip(ctx.cmd,
                           m_workImage.handle(),
                           ctx.denoised->handle(),
                           ctx.extent,
                           VK_ACCESS_2_SHADER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    }

    if (applyHistory) {
        recordTemporalHistoryPass(ctx.cmd, *ctx.denoised, useGradient, hasMotionVectors, grid, guides);
    }

    if (useGradient && applyHistory) {
        recordGradientBlur(ctx.cmd, ctx.extent, iterations, groupsX, groupsY);
    }

    restoreBarriers(ctx.cmd, ctx.hdrBuffer->handle(), ctx.denoised->handle());

    m_firstUse = false;
    m_historyFirstUse = false;
}


void SceneOutputCopyPass::onResize(VkExtent2D extent) noexcept {
    if (extent.width == 0U || extent.height == 0U) {
        return;
    }
    if (createWorkImages(extent)) {
        m_extent = extent;
        m_firstUse = true;
        resetHistory(m_lastResetToken + 1U);
    } else {
        core::Logger::warn("SceneDenoiserPass work image recreate failed; stage disabled for this extent");
    }
}

bool SceneOutputCopyPass::createWorkImages(VkExtent2D extent) noexcept {
    if (m_ctx == nullptr) {
        return false;
    }

    auto history = core::Image::create(*m_ctx,
                                 extent,
                                 VK_FORMAT_R32G32B32A32_SFLOAT,
                                 VK_IMAGE_USAGE_STORAGE_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT,
                                 "harmonia.denoiser.history");
    if (!history) {
        core::Logger::error("SceneDenoiserPass history image creation failed: VkResult {}",
                      static_cast<int>(history.error()));
        return false;
    }

    auto work =
        core::Image::create(*m_ctx,
                      extent,
                      VK_FORMAT_R32G32B32A32_SFLOAT,
                      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT,
                      "harmonia.denoiser.work");
    if (!work) {
        core::Logger::error("SceneDenoiserPass work image creation failed: VkResult {}", static_cast<int>(work.error()));
        return false;
    }

    // 1×1 R32G32F placeholder bound to binding 5 when no real motion vectors are available.
    // Keeps the push-descriptor set valid (correct format) while hasMotionVectors=0
    // ensures the shader never reads from it.
    auto dummyMv = core::Image::create(*m_ctx,
                                 {1U, 1U},
                                 VK_FORMAT_R32G32_SFLOAT,
                                 VK_IMAGE_USAGE_STORAGE_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT,
                                 "harmonia.denoiser.dummyMotionVec");
    if (!dummyMv) {
        core::Logger::error("SceneDenoiserPass dummy motion vector image creation failed: VkResult {}",
                      static_cast<int>(dummyMv.error()));
        return false;
    }

    // A-SVGF gradient/variance buffers (bindings 6/7). Full-res R32G32F, same
    // lifetime as the history image; STORAGE for the compute passes plus
    // TRANSFER for the end-of-frame gradient→prev copy. Only allocated when the
    // gradient path is enabled — otherwise the 1×1 dummy below is bound so the
    // descriptors stay valid without wasting a full-res allocation.
    core::Image gradient{};
    core::Image prevGradient{};
    if (m_settings.useGradient) {
        auto grad = core::Image::create(*m_ctx,
                                  extent,
                                  VK_FORMAT_R32G32_SFLOAT,
                                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT,
                                  "harmonia.denoiser.gradient");
        if (!grad) {
            core::Logger::error("SceneDenoiserPass gradient image creation failed: VkResult {}",
                          static_cast<int>(grad.error()));
            return false;
        }
        auto prevGrad = core::Image::create(*m_ctx,
                                      extent,
                                      VK_FORMAT_R32G32_SFLOAT,
                                      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                      VK_IMAGE_ASPECT_COLOR_BIT,
                                      "harmonia.denoiser.prevGradient");
        if (!prevGrad) {
            core::Logger::error("SceneDenoiserPass prev-gradient image creation failed: VkResult {}",
                          static_cast<int>(prevGrad.error()));
            return false;
        }
        gradient = std::move(*grad);
        prevGradient = std::move(*prevGrad);
    }

    // 1×1 R32G32F placeholder bound to bindings 6/7 when the gradient path is
    // disabled; keeps the push-descriptor set valid without any real read.
    auto dummyGrad = core::Image::create(*m_ctx,
                                   {1U, 1U},
                                   VK_FORMAT_R32G32_SFLOAT,
                                   VK_IMAGE_USAGE_STORAGE_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT,
                                   "harmonia.denoiser.dummyGradient");
    if (!dummyGrad) {
        core::Logger::error("SceneDenoiserPass dummy gradient image creation failed: VkResult {}",
                      static_cast<int>(dummyGrad.error()));
        return false;
    }

    m_historyImage = std::move(*history);
    m_workImage = std::move(*work);
    m_dummyMotionVectors = std::move(*dummyMv);
    m_gradientImage = std::move(gradient);
    m_prevGradientImage = std::move(prevGradient);
    m_dummyGradient = std::move(*dummyGrad);
    return true;
}


void SceneOutputCopyPass::resetHistory(std::uint64_t resetToken) noexcept {
    m_lastResetToken = resetToken;
    m_historyFirstUse = true;
}

} // namespace harmonia::pipeline
