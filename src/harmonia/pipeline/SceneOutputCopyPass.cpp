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
        ctx.hdrBuffer == nullptr || ctx.denoised == nullptr || !m_historyImage.isValid() || !m_workImage.isValid()) {
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
    };
    pipelineBarrier(ctx.cmd, preComputeBarriers);

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

    const bool applyHistory = m_settings.useHistory && ctx.fixedView;
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

    m_historyImage = std::move(*history);
    m_workImage = std::move(*work);
    return true;
}

void SceneOutputCopyPass::resetHistory(uint64_t resetToken) noexcept {
    m_lastResetToken = resetToken;
    m_historyFirstUse = true;
}

void SceneOutputCopyPass::destroy() noexcept {
    m_historyImage = {};
    m_workImage = {};
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
