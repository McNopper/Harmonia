#include "harmonia/pipeline/SceneOutputCopyPass.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <utility>

#include "harmonia/core/Barrier.hpp"
#include "harmonia/core/Logger.hpp"
#include "harmonia/core/ShaderModule.hpp"
#include "harmonia/pipeline/PassContext.hpp"

namespace harmonia {

namespace {
struct alignas(16) DenoiserPushConstants {
    float strength = 0.45F;
    float historyBlend = 0.15F;
    uint32_t hasNormalGuide = 0U;
    uint32_t hasDepthGuide = 0U;
    uint32_t applyHistory = 0U;
    uint32_t historyFirstUse = 1U;
    uint32_t iterations = 1U;
    uint32_t passIndex = 0U;
    uint32_t hasMotionVectors = 0U;  ///< 1 when ctx.motionVectorView is populated
    // ── A-SVGF (A2) ───────────────────────────────────────────────────────────
    uint32_t computeGradient = 0U;      ///< temporal pass computes gradient + variance
    uint32_t gradientFilterMode = 0U;   ///< 1 = this dispatch is the gradient à-trous blur pass
    uint32_t gradientFilterPass = 0U;   ///< gradient à-trous pass index (tap spacing)
    uint32_t hasGradientVariance = 0U;  ///< gGradientVariance holds valid gradient/variance
    float gradientAlpha = 0.2F;         ///< temporal blend factor for the gradient
};

[[nodiscard]] float clamp01(float value) noexcept {
    return std::clamp(value, 0.0F, 1.0F);
}

[[nodiscard]] uint32_t clampIterations(uint32_t iterations) noexcept {
    return std::clamp(iterations, 1U, 8U);
}

} // namespace

std::expected<SceneOutputCopyPass, VkResult> SceneOutputCopyPass::create(const DeviceContext& ctx,
                                                                          VkExtent2D extent,
                                                                          const std::filesystem::path& computeSpvPath,
                                                                          Settings settings) {
    if (!ctx.isValid() || extent.width == 0U || extent.height == 0U) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    settings.strength = clamp01(settings.strength);
    settings.historyBlend = clamp01(settings.historyBlend);
    settings.iterations = clampIterations(settings.iterations);

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
        .bindingCount = static_cast<uint32_t>(bindings.size()),
        .pBindings = bindings.data(),
    };

    SceneOutputCopyPass pass;
    pass.m_ctx = &ctx;
    pass.m_extent = extent;
    pass.m_settings = settings;

    if (const VkResult result = vkCreateDescriptorSetLayout(ctx.device, &setLayoutInfo, nullptr, &pass.m_setLayout);
        result != VK_SUCCESS) {
        return std::unexpected(result);
    }

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
        .pSetLayouts = &pass.m_setLayout,
        .pushConstantRangeCount = 1U,
        .pPushConstantRanges = &pushRange,
    };
    if (const VkResult result = vkCreatePipelineLayout(ctx.device, &pipelineLayoutInfo, nullptr, &pass.m_pipelineLayout);
        result != VK_SUCCESS) {
        return std::unexpected(result);
    }

    const VkSamplerCreateInfo samplerInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0U,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .mipLodBias = 0.0F,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 1.0F,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .minLod = 0.0F,
        .maxLod = 0.0F,
        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    };
    if (const VkResult result = vkCreateSampler(ctx.device, &samplerInfo, nullptr, &pass.m_guideSampler);
        result != VK_SUCCESS) {
        return std::unexpected(result);
    }

    auto module = createShaderModule(ctx.device, computeSpvPath);
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
    const VkResult pipelineResult =
        vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pass.m_pipeline);
    vkDestroyShaderModule(ctx.device, *module, nullptr);
    if (pipelineResult != VK_SUCCESS) {
        return std::unexpected(pipelineResult);
    }

    if (!pass.createWorkImages(extent)) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    ctx.setDebugName(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
                     reinterpret_cast<uint64_t>(pass.m_setLayout),
                     "harmonia.denoiser.setLayout");
    ctx.setDebugName(VK_OBJECT_TYPE_PIPELINE_LAYOUT,
                     reinterpret_cast<uint64_t>(pass.m_pipelineLayout),
                     "harmonia.denoiser.pipelineLayout");
    ctx.setDebugName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(pass.m_pipeline), "harmonia.denoiser.pipeline");
    ctx.setDebugName(VK_OBJECT_TYPE_SAMPLER, reinterpret_cast<uint64_t>(pass.m_guideSampler), "harmonia.denoiser.sampler");
    return pass;
}

SceneOutputCopyPass::SceneOutputCopyPass(SceneOutputCopyPass&& other) noexcept
    : m_ctx(other.m_ctx),
      m_pipeline(std::exchange(other.m_pipeline, VK_NULL_HANDLE)),
      m_pipelineLayout(std::exchange(other.m_pipelineLayout, VK_NULL_HANDLE)),
      m_setLayout(std::exchange(other.m_setLayout, VK_NULL_HANDLE)),
      m_guideSampler(std::exchange(other.m_guideSampler, VK_NULL_HANDLE)),
      m_historyImage(std::move(other.m_historyImage)),
      m_workImage(std::move(other.m_workImage)),
      m_dummyMotionVectors(std::move(other.m_dummyMotionVectors)),
      m_gradientImage(std::move(other.m_gradientImage)),
      m_prevGradientImage(std::move(other.m_prevGradientImage)),
      m_dummyGradient(std::move(other.m_dummyGradient)),
      m_settings(other.m_settings),
      m_extent(other.m_extent),
      m_lastResetToken(other.m_lastResetToken),
      m_firstUse(other.m_firstUse),
      m_historyFirstUse(other.m_historyFirstUse) {
    other.m_ctx = nullptr;
    other.m_extent = {};
    other.m_settings = {};
    other.m_lastResetToken = 0U;
    other.m_firstUse = true;
    other.m_historyFirstUse = true;
}

SceneOutputCopyPass& SceneOutputCopyPass::operator=(SceneOutputCopyPass&& other) noexcept {
    if (this != &other) {
        destroy();
        m_ctx = other.m_ctx;
        m_pipeline = std::exchange(other.m_pipeline, VK_NULL_HANDLE);
        m_pipelineLayout = std::exchange(other.m_pipelineLayout, VK_NULL_HANDLE);
        m_setLayout = std::exchange(other.m_setLayout, VK_NULL_HANDLE);
        m_guideSampler = std::exchange(other.m_guideSampler, VK_NULL_HANDLE);
        m_historyImage = std::move(other.m_historyImage);
        m_workImage = std::move(other.m_workImage);
        m_dummyMotionVectors = std::move(other.m_dummyMotionVectors);
        m_gradientImage = std::move(other.m_gradientImage);
        m_prevGradientImage = std::move(other.m_prevGradientImage);
        m_dummyGradient = std::move(other.m_dummyGradient);
        m_settings = other.m_settings;
        m_extent = other.m_extent;
        m_lastResetToken = other.m_lastResetToken;
        m_firstUse = other.m_firstUse;
        m_historyFirstUse = other.m_historyFirstUse;

        other.m_ctx = nullptr;
        other.m_extent = {};
        other.m_settings = {};
        other.m_lastResetToken = 0U;
        other.m_firstUse = true;
        other.m_historyFirstUse = true;
    }
    return *this;
}

SceneOutputCopyPass::~SceneOutputCopyPass() noexcept {
    destroy();
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

    if (ctx.accumulationResetToken != m_lastResetToken) {
        resetHistory(ctx.accumulationResetToken);
    }

    // A-SVGF gradient path is active only when enabled, the full-res gradient
    // buffers exist, and temporal history is actually applied (fixed view).
    const bool applyHistory = m_settings.useHistory && ctx.fixedView;
    const bool useGradient = m_settings.useGradient && m_gradientImage.isValid() && m_prevGradientImage.isValid();

    const std::array preComputeBarriers{
        imageBarrier(ctx.hdrBuffer->handle(),
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                     VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT),
        imageBarrier(ctx.denoised->handle(),
                     m_firstUse ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     m_firstUse ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     m_firstUse ? 0U : VK_ACCESS_2_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
        imageBarrier(m_workImage.handle(),
                     m_firstUse ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     m_firstUse ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     m_firstUse ? 0U : VK_ACCESS_2_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
        imageBarrier(m_historyImage.handle(),
                     m_historyFirstUse ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     m_historyFirstUse ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     m_historyFirstUse ? 0U : VK_ACCESS_2_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
        // Dummy 1×1 R32G32F image always bound at binding 5; transition UNDEFINED→GENERAL
        // on first use so the descriptor's imageLayout claim is valid even when no real
        // motion-vector pass has run (hasMotionVectors == 0 prevents any actual read).
        imageBarrier(m_dummyMotionVectors.handle(),
                     m_firstUse ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     m_firstUse ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     m_firstUse ? 0U : (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
    };
    pipelineBarrier(ctx.cmd, preComputeBarriers);

    // Gradient/variance buffers (bindings 6/7) → GENERAL for the compute passes.
    // Real full-res images share the history lifetime (UNDEFINED on first use);
    // when the gradient path is off the 1×1 dummy is transitioned instead so its
    // descriptor binding stays valid without a real read.
    if (useGradient) {
        const std::array gradientBarriers{
            imageBarrier(m_gradientImage.handle(),
                         m_historyFirstUse ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_GENERAL,
                         m_historyFirstUse ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         m_historyFirstUse ? 0U : (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
            imageBarrier(m_prevGradientImage.handle(),
                         m_historyFirstUse ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_GENERAL,
                         m_historyFirstUse ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         m_historyFirstUse ? 0U : (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
        };
        pipelineBarrier(ctx.cmd, gradientBarriers);
    } else {
        const std::array dummyGradientBarrier{
            imageBarrier(m_dummyGradient.handle(),
                         m_firstUse ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_GENERAL,
                         m_firstUse ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         m_firstUse ? 0U : (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
        };
        pipelineBarrier(ctx.cmd, dummyGradientBarrier);
    }

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
    constexpr uint32_t kGroupSize = 8U;
    const uint32_t groupsX = (ctx.extent.width + (kGroupSize - 1U)) / kGroupSize;
    const uint32_t groupsY = (ctx.extent.height + (kGroupSize - 1U)) / kGroupSize;

    const VkImageView normalCandidate =
        ctx.gNormalView != VK_NULL_HANDLE ? ctx.gNormalView : ctx.transparentNormalView;
    const VkImageView depthCandidate =
        ctx.gDepthView != VK_NULL_HANDLE ? ctx.gDepthView : ctx.transparentDepthView;
    const bool hasNormalGuide = normalCandidate != VK_NULL_HANDLE;
    const bool hasDepthGuide = depthCandidate != VK_NULL_HANDLE;
    const VkImageView fallbackGuideView = ctx.hdrBuffer->view();
    const VkImageView normalGuideView = hasNormalGuide ? normalCandidate : fallbackGuideView;
    const VkImageView depthGuideView = hasDepthGuide ? depthCandidate : fallbackGuideView;

    const uint32_t iterations = clampIterations(m_settings.iterations);
    VkImage currentSource = ctx.hdrBuffer->handle();
    VkImageView currentSourceView = ctx.hdrBuffer->view();
    VkImage finalSource = currentSource;

    for (uint32_t passIndex = 0U; passIndex < iterations; ++passIndex) {
        const bool writeToDenoised = ((passIndex & 1U) == 0U);
        const VkImageView dstView = writeToDenoised ? ctx.denoised->view() : m_workImage.view();
        finalSource = writeToDenoised ? ctx.denoised->handle() : m_workImage.handle();

        const VkDescriptorImageInfo srcInfo{
            .sampler = VK_NULL_HANDLE,
            .imageView = currentSourceView,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const VkDescriptorImageInfo dstInfo{
            .sampler = VK_NULL_HANDLE,
            .imageView = dstView,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const VkDescriptorImageInfo normalGuideInfo{
            .sampler = m_guideSampler,
            .imageView = normalGuideView,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const VkDescriptorImageInfo depthGuideInfo{
            .sampler = m_guideSampler,
            .imageView = depthGuideView,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const VkDescriptorImageInfo historyInfo{
            .sampler = VK_NULL_HANDLE,
            .imageView = m_historyImage.view(),
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const VkDescriptorImageInfo motionVecInfo{
            .sampler = VK_NULL_HANDLE,
            .imageView = motionVecView,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const VkDescriptorImageInfo gradientInfo{
            .sampler = VK_NULL_HANDLE,
            .imageView = gradientView,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const VkDescriptorImageInfo prevGradientInfo{
            .sampler = VK_NULL_HANDLE,
            .imageView = prevGradientView,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const std::array writes{
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = VK_NULL_HANDLE,
                .dstBinding = 0,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &srcInfo,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = VK_NULL_HANDLE,
                .dstBinding = 1,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &dstInfo,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = VK_NULL_HANDLE,
                .dstBinding = 2,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &normalGuideInfo,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = VK_NULL_HANDLE,
                .dstBinding = 3,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &depthGuideInfo,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = VK_NULL_HANDLE,
                .dstBinding = 4,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &historyInfo,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = VK_NULL_HANDLE,
                .dstBinding = 5,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &motionVecInfo,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = VK_NULL_HANDLE,
                .dstBinding = 6,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &gradientInfo,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = VK_NULL_HANDLE,
                .dstBinding = 7,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &prevGradientInfo,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            },
        };

        const DenoiserPushConstants push{
            .strength = m_settings.strength,
            .historyBlend = m_settings.historyBlend,
            .hasNormalGuide = hasNormalGuide ? 1U : 0U,
            .hasDepthGuide = hasDepthGuide ? 1U : 0U,
            .applyHistory = 0U,
            .historyFirstUse = m_historyFirstUse ? 1U : 0U,
            .iterations = iterations,
            .passIndex = passIndex,
            .hasMotionVectors = 0U,  // spatial passes never read motion vectors
            .computeGradient = 0U,   // gradient is computed in the temporal pass only
            .gradientFilterMode = 0U,
            .gradientFilterPass = 0U,
            .hasGradientVariance = hasGradientVariance ? 1U : 0U,  // variance-guided luma edge stopping
            .gradientAlpha = m_settings.gradientAlpha,
        };
        vkCmdPushDescriptorSet(ctx.cmd,
                               VK_PIPELINE_BIND_POINT_COMPUTE,
                               m_pipelineLayout,
                               0U,
                               static_cast<uint32_t>(writes.size()),
                               writes.data());
        vkCmdPushConstants(ctx.cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(push), &push);
        vkCmdDispatch(ctx.cmd, groupsX, groupsY, 1U);

        currentSource = writeToDenoised ? ctx.denoised->handle() : m_workImage.handle();
        currentSourceView = writeToDenoised ? ctx.denoised->view() : m_workImage.view();

        if (passIndex + 1U < iterations) {
            const std::array passBarrier{
                imageBarrier(currentSource,
                             VK_IMAGE_LAYOUT_GENERAL,
                             VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_SHADER_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
            };
            pipelineBarrier(ctx.cmd, passBarrier);
        }
    }

    if (finalSource != ctx.denoised->handle()) {
        const std::array copyPrep{
            imageBarrier(m_workImage.handle(),
                         VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                         VK_ACCESS_2_TRANSFER_READ_BIT),
            imageBarrier(ctx.denoised->handle(),
                         VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT),
        };
        pipelineBarrier(ctx.cmd, copyPrep);

        const VkImageCopy copyRegion{
            .srcSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0U, 0U, 1U},
            .srcOffset = VkOffset3D{0, 0, 0},
            .dstSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0U, 0U, 1U},
            .dstOffset = VkOffset3D{0, 0, 0},
            .extent = VkExtent3D{ctx.extent.width, ctx.extent.height, 1U},
        };
        vkCmdCopyImage(ctx.cmd,
                       m_workImage.handle(),
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       ctx.denoised->handle(),
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1U,
                       &copyRegion);

        const std::array copyRestore{
            imageBarrier(m_workImage.handle(),
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                         VK_ACCESS_2_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
            imageBarrier(ctx.denoised->handle(),
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
        };
        pipelineBarrier(ctx.cmd, copyRestore);
    }

    if (applyHistory) {
        const VkDescriptorImageInfo denoisedInfo{
            .sampler = VK_NULL_HANDLE,
            .imageView = ctx.denoised->view(),
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const VkDescriptorImageInfo normalGuideInfo{
            .sampler = m_guideSampler,
            .imageView = normalGuideView,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const VkDescriptorImageInfo depthGuideInfo{
            .sampler = m_guideSampler,
            .imageView = depthGuideView,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const VkDescriptorImageInfo historyInfo{
            .sampler = VK_NULL_HANDLE,
            .imageView = m_historyImage.view(),
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const VkDescriptorImageInfo motionVecHistInfo{
            .sampler = VK_NULL_HANDLE,
            .imageView = motionVecView,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const VkDescriptorImageInfo gradientHistInfo{
            .sampler = VK_NULL_HANDLE,
            .imageView = gradientView,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const VkDescriptorImageInfo prevGradientHistInfo{
            .sampler = VK_NULL_HANDLE,
            .imageView = prevGradientView,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const std::array historyWrites{
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = VK_NULL_HANDLE,
                .dstBinding = 0,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &denoisedInfo,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = VK_NULL_HANDLE,
                .dstBinding = 1,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &denoisedInfo,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = VK_NULL_HANDLE,
                .dstBinding = 2,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &normalGuideInfo,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = VK_NULL_HANDLE,
                .dstBinding = 3,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &depthGuideInfo,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = VK_NULL_HANDLE,
                .dstBinding = 4,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &historyInfo,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = VK_NULL_HANDLE,
                .dstBinding = 5,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &motionVecHistInfo,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = VK_NULL_HANDLE,
                .dstBinding = 6,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &gradientHistInfo,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = VK_NULL_HANDLE,
                .dstBinding = 7,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &prevGradientHistInfo,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            },
        };

        const DenoiserPushConstants push{
            .strength = m_settings.strength,
            .historyBlend = m_settings.historyBlend,
            .hasNormalGuide = hasNormalGuide ? 1U : 0U,
            .hasDepthGuide = hasDepthGuide ? 1U : 0U,
            .applyHistory = 1U,
            .historyFirstUse = m_historyFirstUse ? 1U : 0U,
            .iterations = iterations,
            .passIndex = iterations,
            .hasMotionVectors = hasMotionVectors ? 1U : 0U,
            .computeGradient = useGradient ? 1U : 0U,  // temporal pass writes gradient + variance
            .gradientFilterMode = 0U,
            .gradientFilterPass = 0U,
            .hasGradientVariance = 0U,  // temporal pass writes, does not read, the variance
            .gradientAlpha = m_settings.gradientAlpha,
        };
        vkCmdPushDescriptorSet(ctx.cmd,
                               VK_PIPELINE_BIND_POINT_COMPUTE,
                               m_pipelineLayout,
                               0U,
                               static_cast<uint32_t>(historyWrites.size()),
                               historyWrites.data());
        vkCmdPushConstants(ctx.cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(push), &push);
        vkCmdDispatch(ctx.cmd, groupsX, groupsY, 1U);
    }

    // ── A-SVGF gradient à-trous propagation + prev-frame carry ──────────────────
    // After the temporal pass has written this frame's per-pixel gradient into
    // m_gradientImage, spread it spatially with a few à-trous-spaced Gaussian
    // passes (ping-ponging m_gradientImage ↔ m_prevGradientImage; an even pass
    // count returns the result to m_gradientImage), then copy it into
    // m_prevGradientImage so next frame's temporal smoothing and variance guiding
    // read a defined, consistent value. On first use the temporal pass only seeds
    // the gradient to zero, so the blur is skipped and only the carry copy runs.
    if (useGradient && applyHistory) {
        if (!m_historyFirstUse) {
            constexpr uint32_t kGradientFilterPasses = 2U;  // even → final result lands in m_gradientImage
            for (uint32_t gp = 0U; gp < kGradientFilterPasses; ++gp) {
                const bool srcIsGradient = (gp % 2U) == 0U;
                const VkImageView gradSrcView = srcIsGradient ? m_gradientImage.view() : m_prevGradientImage.view();
                const VkImageView gradDstView = srcIsGradient ? m_prevGradientImage.view() : m_gradientImage.view();
                const VkImage gradSrcImage = srcIsGradient ? m_gradientImage.handle() : m_prevGradientImage.handle();
                const VkImage gradDstImage = srcIsGradient ? m_prevGradientImage.handle() : m_gradientImage.handle();

                const std::array gradPassBarriers{
                    imageBarrier(gradSrcImage,
                                 VK_IMAGE_LAYOUT_GENERAL,
                                 VK_IMAGE_LAYOUT_GENERAL,
                                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                 VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                 VK_ACCESS_2_SHADER_READ_BIT),
                    imageBarrier(gradDstImage,
                                 VK_IMAGE_LAYOUT_GENERAL,
                                 VK_IMAGE_LAYOUT_GENERAL,
                                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                 VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                 VK_ACCESS_2_SHADER_WRITE_BIT),
                };
                pipelineBarrier(ctx.cmd, gradPassBarriers);

                // binding 6 = destination (gGradientVariance, written);
                // binding 7 = source (gPrevGradientVariance, read neighbourhood).
                const VkDescriptorImageInfo gradDstInfo{
                    .sampler = VK_NULL_HANDLE,
                    .imageView = gradDstView,
                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                };
                const VkDescriptorImageInfo gradSrcInfo{
                    .sampler = VK_NULL_HANDLE,
                    .imageView = gradSrcView,
                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                };
                const std::array gradWrites{
                    VkWriteDescriptorSet{
                        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .pNext = nullptr,
                        .dstSet = VK_NULL_HANDLE,
                        .dstBinding = 6,
                        .dstArrayElement = 0,
                        .descriptorCount = 1,
                        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                        .pImageInfo = &gradDstInfo,
                        .pBufferInfo = nullptr,
                        .pTexelBufferView = nullptr,
                    },
                    VkWriteDescriptorSet{
                        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .pNext = nullptr,
                        .dstSet = VK_NULL_HANDLE,
                        .dstBinding = 7,
                        .dstArrayElement = 0,
                        .descriptorCount = 1,
                        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                        .pImageInfo = &gradSrcInfo,
                        .pBufferInfo = nullptr,
                        .pTexelBufferView = nullptr,
                    },
                };

                DenoiserPushConstants gradPush{};
                gradPush.strength = m_settings.strength;
                gradPush.historyBlend = m_settings.historyBlend;
                gradPush.iterations = iterations;
                gradPush.passIndex = iterations;
                gradPush.gradientFilterMode = 1U;
                gradPush.gradientFilterPass = gp;
                gradPush.gradientAlpha = m_settings.gradientAlpha;

                vkCmdPushDescriptorSet(ctx.cmd,
                                       VK_PIPELINE_BIND_POINT_COMPUTE,
                                       m_pipelineLayout,
                                       0U,
                                       static_cast<uint32_t>(gradWrites.size()),
                                       gradWrites.data());
                vkCmdPushConstants(ctx.cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(gradPush), &gradPush);
                vkCmdDispatch(ctx.cmd, groupsX, groupsY, 1U);
            }
        }

        // Carry the (possibly blurred) gradient into the previous-frame buffer.
        const std::array gradCopyPrep{
            imageBarrier(m_gradientImage.handle(),
                         VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                         VK_ACCESS_2_TRANSFER_READ_BIT),
            imageBarrier(m_prevGradientImage.handle(),
                         VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT),
        };
        pipelineBarrier(ctx.cmd, gradCopyPrep);

        const VkImageCopy gradCopyRegion{
            .srcSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0U, 0U, 1U},
            .srcOffset = VkOffset3D{0, 0, 0},
            .dstSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0U, 0U, 1U},
            .dstOffset = VkOffset3D{0, 0, 0},
            .extent = VkExtent3D{ctx.extent.width, ctx.extent.height, 1U},
        };
        vkCmdCopyImage(ctx.cmd,
                       m_gradientImage.handle(),
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       m_prevGradientImage.handle(),
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1U,
                       &gradCopyRegion);

        const std::array gradCopyRestore{
            imageBarrier(m_gradientImage.handle(),
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                         VK_ACCESS_2_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
            imageBarrier(m_prevGradientImage.handle(),
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
        };
        pipelineBarrier(ctx.cmd, gradCopyRestore);
    }

    const std::array restoreBarriers{
        imageBarrier(ctx.hdrBuffer->handle(),
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_2_TRANSFER_WRITE_BIT),
        imageBarrier(ctx.denoised->handle(),
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_2_TRANSFER_WRITE_BIT),
        imageBarrier(m_workImage.handle(),
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
        imageBarrier(m_historyImage.handle(),
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
    };
    pipelineBarrier(ctx.cmd, restoreBarriers);

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
        Logger::warn("SceneDenoiserPass work image recreate failed; stage disabled for this extent");
    }
}

bool SceneOutputCopyPass::createWorkImages(VkExtent2D extent) noexcept {
    if (m_ctx == nullptr) {
        return false;
    }

    auto history = Image::create(*m_ctx,
                                 extent,
                                 VK_FORMAT_R32G32B32A32_SFLOAT,
                                 VK_IMAGE_USAGE_STORAGE_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT,
                                 "harmonia.denoiser.history");
    if (!history) {
        Logger::error("SceneDenoiserPass history image creation failed: VkResult {}", static_cast<int>(history.error()));
        return false;
    }

    auto work = Image::create(*m_ctx,
                              extent,
                              VK_FORMAT_R32G32B32A32_SFLOAT,
                              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT,
                              "harmonia.denoiser.work");
    if (!work) {
        Logger::error("SceneDenoiserPass work image creation failed: VkResult {}", static_cast<int>(work.error()));
        return false;
    }

    // 1×1 R32G32F placeholder bound to binding 5 when no real motion vectors are available.
    // Keeps the push-descriptor set valid (correct format) while hasMotionVectors=0
    // ensures the shader never reads from it.
    auto dummyMv = Image::create(*m_ctx,
                                 {1U, 1U},
                                 VK_FORMAT_R32G32_SFLOAT,
                                 VK_IMAGE_USAGE_STORAGE_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT,
                                 "harmonia.denoiser.dummyMotionVec");
    if (!dummyMv) {
        Logger::error("SceneDenoiserPass dummy motion vector image creation failed: VkResult {}", static_cast<int>(dummyMv.error()));
        return false;
    }

    // A-SVGF gradient/variance buffers (bindings 6/7). Full-res R32G32F, same
    // lifetime as the history image; STORAGE for the compute passes plus
    // TRANSFER for the end-of-frame gradient→prev copy. Only allocated when the
    // gradient path is enabled — otherwise the 1×1 dummy below is bound so the
    // descriptors stay valid without wasting a full-res allocation.
    Image gradient{};
    Image prevGradient{};
    if (m_settings.useGradient) {
        auto grad = Image::create(*m_ctx,
                                  extent,
                                  VK_FORMAT_R32G32_SFLOAT,
                                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT,
                                  "harmonia.denoiser.gradient");
        if (!grad) {
            Logger::error("SceneDenoiserPass gradient image creation failed: VkResult {}", static_cast<int>(grad.error()));
            return false;
        }
        auto prevGrad = Image::create(*m_ctx,
                                      extent,
                                      VK_FORMAT_R32G32_SFLOAT,
                                      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                      VK_IMAGE_ASPECT_COLOR_BIT,
                                      "harmonia.denoiser.prevGradient");
        if (!prevGrad) {
            Logger::error("SceneDenoiserPass prev-gradient image creation failed: VkResult {}", static_cast<int>(prevGrad.error()));
            return false;
        }
        gradient = std::move(*grad);
        prevGradient = std::move(*prevGrad);
    }

    // 1×1 R32G32F placeholder bound to bindings 6/7 when the gradient path is
    // disabled; keeps the push-descriptor set valid without any real read.
    auto dummyGrad = Image::create(*m_ctx,
                                   {1U, 1U},
                                   VK_FORMAT_R32G32_SFLOAT,
                                   VK_IMAGE_USAGE_STORAGE_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT,
                                   "harmonia.denoiser.dummyGradient");
    if (!dummyGrad) {
        Logger::error("SceneDenoiserPass dummy gradient image creation failed: VkResult {}", static_cast<int>(dummyGrad.error()));
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

void SceneOutputCopyPass::resetHistory(uint64_t resetToken) noexcept {
    m_lastResetToken = resetToken;
    m_historyFirstUse = true;
}

void SceneOutputCopyPass::destroy() noexcept {
    m_historyImage = {};
    m_workImage = {};
    m_dummyMotionVectors = {};
    m_gradientImage = {};
    m_prevGradientImage = {};
    m_dummyGradient = {};
    if (m_ctx != nullptr) {
        if (m_pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(m_ctx->device, m_pipeline, nullptr);
            m_pipeline = VK_NULL_HANDLE;
        }
        if (m_pipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(m_ctx->device, m_pipelineLayout, nullptr);
            m_pipelineLayout = VK_NULL_HANDLE;
        }
        if (m_setLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(m_ctx->device, m_setLayout, nullptr);
            m_setLayout = VK_NULL_HANDLE;
        }
        if (m_guideSampler != VK_NULL_HANDLE) {
            vkDestroySampler(m_ctx->device, m_guideSampler, nullptr);
            m_guideSampler = VK_NULL_HANDLE;
        }
    }
    m_ctx = nullptr;
    m_extent = {};
    m_settings = {};
    m_lastResetToken = 0U;
    m_firstUse = true;
    m_historyFirstUse = true;
}

} // namespace harmonia
