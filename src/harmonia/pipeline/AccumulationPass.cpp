#include "harmonia/pipeline/AccumulationPass.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <utility>

#include "harmonia/core/Barrier.hpp"
#include "harmonia/core/Logger.hpp"
#include "harmonia/core/ShaderModule.hpp"
#include "harmonia/pipeline/PassContext.hpp"

namespace harmonia {

namespace {
struct alignas(4) AccumulationPushConstants {
    std::uint32_t sampleCount = 0;
};
} // namespace

std::expected<AccumulationPass, VkResult>
AccumulationPass::create(const DeviceContext& ctx, VkExtent2D extent, const std::filesystem::path& computeSpvPath) {
    if (!ctx.isValid() || extent.width == 0U || extent.height == 0U) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

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
    };
    const VkDescriptorSetLayoutCreateInfo setLayoutInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT,
        .bindingCount = static_cast<std::uint32_t>(bindings.size()),
        .pBindings = bindings.data(),
    };

    AccumulationPass pass;
    pass.m_ctx = &ctx;
    pass.m_extent = extent;
    if (const VkResult result = vkCreateDescriptorSetLayout(ctx.device, &setLayoutInfo, nullptr, &pass.m_setLayout);
        result != VK_SUCCESS) {
        return std::unexpected(result);
    }

    const VkPushConstantRange pushRange{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(AccumulationPushConstants),
    };
    const VkPipelineLayoutCreateInfo pipelineLayoutInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = &pass.m_setLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushRange,
    };
    if (const VkResult result =
            vkCreatePipelineLayout(ctx.device, &pipelineLayoutInfo, nullptr, &pass.m_pipelineLayout);
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
        .flags = 0,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = *module,
        .pName = "main",
        .pSpecializationInfo = nullptr,
    };
    const VkComputePipelineCreateInfo pipelineInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
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

    if (!pass.createHistoryImage(extent)) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    ctx.setDebugName(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
                     reinterpret_cast<std::uint64_t>(pass.m_setLayout),
                     "harmonia.accum.setLayout");
    ctx.setDebugName(VK_OBJECT_TYPE_PIPELINE_LAYOUT,
                     reinterpret_cast<std::uint64_t>(pass.m_pipelineLayout),
                     "harmonia.accum.pipelineLayout");
    ctx.setDebugName(
        VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<std::uint64_t>(pass.m_pipeline), "harmonia.accum.pipeline");
    return pass;
}

AccumulationPass::AccumulationPass(AccumulationPass&& other) noexcept
    : m_ctx(other.m_ctx),
      m_pipeline(std::exchange(other.m_pipeline, VK_NULL_HANDLE)),
      m_pipelineLayout(std::exchange(other.m_pipelineLayout, VK_NULL_HANDLE)),
      m_setLayout(std::exchange(other.m_setLayout, VK_NULL_HANDLE)),
      m_historyImage(std::move(other.m_historyImage)),
      m_extent(other.m_extent),
      m_sampleCount(other.m_sampleCount),
      m_lastResetToken(other.m_lastResetToken),
      m_historyFirstUse(other.m_historyFirstUse) {
    other.m_ctx = nullptr;
    other.m_extent = {};
    other.m_sampleCount = 0;
    other.m_lastResetToken = 0;
    other.m_historyFirstUse = true;
}

AccumulationPass& AccumulationPass::operator=(AccumulationPass&& other) noexcept {
    if (this != &other) {
        destroy();
        m_ctx = other.m_ctx;
        m_pipeline = std::exchange(other.m_pipeline, VK_NULL_HANDLE);
        m_pipelineLayout = std::exchange(other.m_pipelineLayout, VK_NULL_HANDLE);
        m_setLayout = std::exchange(other.m_setLayout, VK_NULL_HANDLE);
        m_historyImage = std::move(other.m_historyImage);
        m_extent = other.m_extent;
        m_sampleCount = other.m_sampleCount;
        m_lastResetToken = other.m_lastResetToken;
        m_historyFirstUse = other.m_historyFirstUse;

        other.m_ctx = nullptr;
        other.m_extent = {};
        other.m_sampleCount = 0;
        other.m_lastResetToken = 0;
        other.m_historyFirstUse = true;
    }
    return *this;
}

AccumulationPass::~AccumulationPass() noexcept {
    destroy();
}

void AccumulationPass::record(const PassContext& ctx) noexcept {
    if (m_ctx == nullptr || m_pipeline == VK_NULL_HANDLE || m_pipelineLayout == VK_NULL_HANDLE ||
        m_setLayout == VK_NULL_HANDLE || ctx.cmd == VK_NULL_HANDLE || ctx.hdrBuffer == nullptr ||
        !m_historyImage.isValid()) {
        return;
    }

    if (!ctx.fixedView) {
        resetHistory(ctx.accumulationResetToken);
        return;
    }

    if (ctx.extent.width == 0U || ctx.extent.height == 0U) {
        return;
    }
    if (ctx.extent.width != m_extent.width || ctx.extent.height != m_extent.height) {
        onResize(ctx.extent);
        if (!m_historyImage.isValid()) {
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
                     VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT),
        imageBarrier(m_historyImage.handle(),
                     m_historyFirstUse ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     m_historyFirstUse ? VK_PIPELINE_STAGE_2_NONE
                                       : (VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT),
                     m_historyFirstUse ? 0U : (VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT),
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
    };
    pipelineBarrier(ctx.cmd, preComputeBarriers);

    const VkDescriptorImageInfo hdrInfo{
        .sampler = VK_NULL_HANDLE,
        .imageView = ctx.hdrBuffer->view(),
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
            .pImageInfo = &hdrInfo,
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
            .pImageInfo = &historyInfo,
            .pBufferInfo = nullptr,
            .pTexelBufferView = nullptr,
        },
    };

    const AccumulationPushConstants push{
        .sampleCount = m_sampleCount,
    };

    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdPushDescriptorSet(ctx.cmd,
                           VK_PIPELINE_BIND_POINT_COMPUTE,
                           m_pipelineLayout,
                           0,
                           static_cast<std::uint32_t>(writes.size()),
                           writes.data());
    vkCmdPushConstants(ctx.cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);

    constexpr std::uint32_t kGroupSize = 8U;
    const std::uint32_t groupsX = (ctx.extent.width + (kGroupSize - 1U)) / kGroupSize;
    const std::uint32_t groupsY = (ctx.extent.height + (kGroupSize - 1U)) / kGroupSize;
    vkCmdDispatch(ctx.cmd, groupsX, groupsY, 1);

    const std::array postComputeBarriers{
        imageBarrier(m_historyImage.handle(),
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT),
        imageBarrier(ctx.hdrBuffer->handle(),
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT),
    };
    pipelineBarrier(ctx.cmd, postComputeBarriers);

    const VkImageCopy copyRegion{
        .srcSubresource =
            VkImageSubresourceLayers{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .srcOffset = VkOffset3D{0, 0, 0},
        .dstSubresource =
            VkImageSubresourceLayers{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .dstOffset = VkOffset3D{0, 0, 0},
        .extent = VkExtent3D{ctx.extent.width, ctx.extent.height, 1U},
    };
    vkCmdCopyImage(ctx.cmd,
                   m_historyImage.handle(),
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   ctx.hdrBuffer->handle(),
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1,
                   &copyRegion);

    const std::array restoreBarriers{
        imageBarrier(m_historyImage.handle(),
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
        imageBarrier(ctx.hdrBuffer->handle(),
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT),
    };
    pipelineBarrier(ctx.cmd, restoreBarriers);

    if (m_sampleCount < std::numeric_limits<std::uint32_t>::max()) {
        ++m_sampleCount;
    }
    m_historyFirstUse = false;
}

void AccumulationPass::onResize(VkExtent2D extent) noexcept {
    if (extent.width == 0U || extent.height == 0U) {
        return;
    }
    if (createHistoryImage(extent)) {
        m_extent = extent;
        resetHistory(m_lastResetToken + 1U);
    } else {
        Logger::warn("AccumulationPass history image recreate failed; accumulation disabled for this extent");
    }
}

bool AccumulationPass::createHistoryImage(VkExtent2D extent) noexcept {
    if (m_ctx == nullptr) {
        return false;
    }
    auto history = Image::create(*m_ctx,
                                 extent,
                                 VK_FORMAT_R32G32B32A32_SFLOAT,
                                 VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT,
                                 "harmonia.accum.history");
    if (!history) {
        Logger::error("AccumulationPass history image creation failed: VkResult {}", static_cast<int>(history.error()));
        return false;
    }
    m_historyImage = std::move(*history);
    return true;
}

void AccumulationPass::resetHistory(std::uint64_t resetToken) noexcept {
    m_sampleCount = 0;
    m_historyFirstUse = true;
    m_lastResetToken = resetToken;
}

void AccumulationPass::destroy() noexcept {
    m_historyImage = {};
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
    }
    m_ctx = nullptr;
    m_extent = {};
    m_sampleCount = 0;
    m_lastResetToken = 0;
    m_historyFirstUse = true;
}

} // namespace harmonia
