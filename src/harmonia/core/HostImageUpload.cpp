#include "harmonia/core/HostImageUpload.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "harmonia/core/CommandPool.hpp"

namespace harmonia::core {

std::expected<HostImageUpload, VkResult>
uploadHostImage(const DeviceContext& ctx, const CommandPool& cmdPool, const HostImageUploadDesc& desc) {
    // PERF8 dependency (RTXPT §7 conservative MIPs): generate a full GPU mip chain
    // at upload time so explicit-SampleLevel consumers (the Theia GI material-LOD
    // tier) and implicit-LOD consumers (the raster forward pass) have coarser levels
    // to select. Guarded by the format's blit capabilities — formats without
    // linear-blit support keep today's single level (behaviour unchanged).
    // Mip 0 content is untouched, and the shared scene sampler (texsampler.slang)
    // fetches EXPLICIT lod 0, so existing renderer output is bit-stable.
    std::uint32_t mipLevels = 1U;
    if (ctx.isValid() && ctx.physicalDevice != VK_NULL_HANDLE) {
        VkFormatProperties formatProps{};
        vkGetPhysicalDeviceFormatProperties(ctx.physicalDevice, desc.format, &formatProps);
        constexpr VkFormatFeatureFlags kBlitFeatures = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                                       VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if ((formatProps.optimalTilingFeatures & kBlitFeatures) == kBlitFeatures) {
            const std::uint32_t maxDim = std::max(desc.extent.width, desc.extent.height);
            mipLevels = 1U + static_cast<std::uint32_t>(std::floor(std::log2(static_cast<double>(maxDim))));
        }
    }

    VkImageUsageFlags usage = VK_IMAGE_USAGE_HOST_TRANSFER_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (mipLevels > 1U) {
        // The blit chain READS each source level and WRITES the next one — both
        // transfer usages are required (validation-enforced; a missing TRANSFER_DST
        // makes every downsample blit VUID-invalid and leaves higher mips undefined).
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }

    auto image = Image::create(ctx, desc.extent, desc.format, usage, VK_IMAGE_ASPECT_COLOR_BIT, desc.name, mipLevels);
    if (!image) {
        return std::unexpected(image.error());
    }

    // Vulkan 1.4 hostImageCopy: stream host pixels straight into the (optimal-tiling) image
    // with vkCopyMemoryToImage — no staging buffer, no device-side copy. Layout path:
    // UNDEFINED -> GENERAL (queue), host copy into GENERAL, GENERAL -> SHADER_READ_ONLY (queue).
    {
        auto cmd = cmdPool.beginOneShot();
        if (!cmd) {
            return std::unexpected(cmd.error());
        }
        image->transition(*cmd,
                          VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_2_NONE,
                          0,
                          VK_PIPELINE_STAGE_2_HOST_BIT,
                          VK_ACCESS_2_HOST_WRITE_BIT);
        if (const VkResult result = cmdPool.endOneShot(*cmd); result != VK_SUCCESS) {
            return std::unexpected(result);
        }
    }

    const VkMemoryToImageCopy region{
        .sType = VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY,
        .pNext = nullptr,
        .pHostPointer = desc.pixels,
        .memoryRowLength = 0,
        .memoryImageHeight = 0,
        .imageSubresource =
            VkImageSubresourceLayers{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .imageOffset = VkOffset3D{0, 0, 0},
        .imageExtent = VkExtent3D{desc.extent.width, desc.extent.height, 1},
    };
    const VkCopyMemoryToImageInfo copyInfo{
        .sType = VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .dstImage = image->handle(),
        .dstImageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .regionCount = 1,
        .pRegions = &region,
    };
    if (const VkResult result = vkCopyMemoryToImage(ctx.device, &copyInfo); result != VK_SUCCESS) {
        return std::unexpected(result);
    }

    {
        auto cmd = cmdPool.beginOneShot();
        if (!cmd) {
            return std::unexpected(cmd.error());
        }

        if (mipLevels > 1U) {
            // Host copy (mip 0) -> first blit read.
            const VkImageMemoryBarrier2 hostToTransfer{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .pNext = nullptr,
                .srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                .srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image->handle(),
                .subresourceRange =
                    VkImageSubresourceRange{
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .baseMipLevel = 0U,
                        .levelCount = 1U,
                        .baseArrayLayer = 0U,
                        .layerCount = 1U,
                    },
            };
            const VkDependencyInfo hostDep{
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .pNext = nullptr,
                .dependencyFlags = 0U,
                .memoryBarrierCount = 0U,
                .pMemoryBarriers = nullptr,
                .bufferMemoryBarrierCount = 0U,
                .pBufferMemoryBarriers = nullptr,
                .imageMemoryBarrierCount = 1U,
                .pImageMemoryBarriers = &hostToTransfer,
            };
            vkCmdPipelineBarrier2(*cmd, &hostDep);

            // Linear downsample chain: mip i-1 -> mip i (whole image stays GENERAL).
            for (std::uint32_t level = 1U; level < mipLevels; ++level) {
                const std::uint32_t srcW = std::max(1U, desc.extent.width >> (level - 1U));
                const std::uint32_t srcH = std::max(1U, desc.extent.height >> (level - 1U));
                const std::uint32_t dstW = std::max(1U, desc.extent.width >> level);
                const std::uint32_t dstH = std::max(1U, desc.extent.height >> level);
                const VkImageBlit blit{
                    .srcSubresource =
                        VkImageSubresourceLayers{
                            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                            .mipLevel = level - 1U,
                            .baseArrayLayer = 0U,
                            .layerCount = 1U,
                        },
                    .srcOffsets = {{0, 0, 0}, {static_cast<std::int32_t>(srcW), static_cast<std::int32_t>(srcH), 1}},
                    .dstSubresource =
                        VkImageSubresourceLayers{
                            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                            .mipLevel = level,
                            .baseArrayLayer = 0U,
                            .layerCount = 1U,
                        },
                    .dstOffsets = {{0, 0, 0}, {static_cast<std::int32_t>(dstW), static_cast<std::int32_t>(dstH), 1}},
                };
                vkCmdBlitImage(*cmd,
                               image->handle(),
                               VK_IMAGE_LAYOUT_GENERAL,
                               image->handle(),
                               VK_IMAGE_LAYOUT_GENERAL,
                               1U,
                               &blit,
                               VK_FILTER_LINEAR);

                // Sequential dependency: this blit's write -> the next blit's read.
                if (level + 1U < mipLevels) {
                    const VkImageMemoryBarrier2 levelBarrier{
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                        .pNext = nullptr,
                        .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                        .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = image->handle(),
                        .subresourceRange =
                            VkImageSubresourceRange{
                                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                .baseMipLevel = 0U,
                                .levelCount = mipLevels,
                                .baseArrayLayer = 0U,
                                .layerCount = 1U,
                            },
                    };
                    const VkDependencyInfo levelDep{
                        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                        .pNext = nullptr,
                        .dependencyFlags = 0U,
                        .memoryBarrierCount = 0U,
                        .pMemoryBarriers = nullptr,
                        .bufferMemoryBarrierCount = 0U,
                        .pBufferMemoryBarriers = nullptr,
                        .imageMemoryBarrierCount = 1U,
                        .pImageMemoryBarriers = &levelBarrier,
                    };
                    vkCmdPipelineBarrier2(*cmd, &levelDep);
                }
            }
        }

        image->transition(*cmd,
                          VK_IMAGE_LAYOUT_GENERAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          mipLevels > 1U ? (VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT)
                                         : VK_PIPELINE_STAGE_2_HOST_BIT,
                          mipLevels > 1U ? (VK_ACCESS_2_HOST_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT)
                                         : VK_ACCESS_2_HOST_WRITE_BIT,
                          desc.finalStages,
                          desc.finalAccess);
        if (const VkResult result = cmdPool.endOneShot(*cmd); result != VK_SUCCESS) {
            return std::unexpected(result);
        }
    }

    const VkSamplerCreateInfo samplerInfo = makeSamplerCreateInfo(desc.sampler);
    VkSampler sampler = VK_NULL_HANDLE;
    if (const VkResult result = vkCreateSampler(ctx.device, &samplerInfo, nullptr, &sampler); result != VK_SUCCESS) {
        return std::unexpected(result);
    }

    return HostImageUpload{
        .image = std::move(*image),
        .sampler = UniqueSampler{ctx.device, sampler},
    };
}

} // namespace harmonia::core
