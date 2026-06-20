#include "harmonia/pipeline/SceneOutputCopyPass.hpp"

#include <array>

#include "harmonia/core/Barrier.hpp"
#include "harmonia/core/Image.hpp"
#include "harmonia/pipeline/PassContext.hpp"

namespace harmonia {

void SceneOutputCopyPass::record(const PassContext& ctx) noexcept {
    if (ctx.cmd == VK_NULL_HANDLE || ctx.hdrBuffer == nullptr || ctx.denoised == nullptr) {
        return;
    }

    const VkImageLayout dstOldLayout = m_firstUse ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
    const std::array barriers{
        imageBarrier(ctx.hdrBuffer->handle(),
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                     VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT),
        imageBarrier(ctx.denoised->handle(),
                     dstOldLayout,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_NONE,
                     0,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT),
    };
    pipelineBarrier(ctx.cmd, barriers);

    const VkExtent2D extent = ctx.hdrBuffer->extent();
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
        .extent = VkExtent3D{extent.width, extent.height, 1U},
    };
    vkCmdCopyImage(ctx.cmd,
                   ctx.hdrBuffer->handle(),
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   ctx.denoised->handle(),
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1,
                   &copyRegion);

    const std::array restoreBarriers{
        imageBarrier(ctx.hdrBuffer->handle(),
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                     VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT |
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT),
        imageBarrier(ctx.denoised->handle(),
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                     VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT |
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT),
    };
    pipelineBarrier(ctx.cmd, restoreBarriers);

    m_firstUse = false;
}

void SceneOutputCopyPass::onResize(VkExtent2D extent) noexcept {
    m_firstUse = true;
    m_extent = extent;
}

} // namespace harmonia
