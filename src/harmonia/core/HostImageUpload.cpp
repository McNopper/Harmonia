#include "harmonia/core/HostImageUpload.hpp"

#include "harmonia/core/CommandPool.hpp"

namespace harmonia {

std::expected<HostImageUpload, VkResult> uploadHostImage(const DeviceContext& ctx,
                                                         const CommandPool& cmdPool,
                                                         const HostImageUploadDesc& desc) {
    auto image = Image::create(ctx,
                               desc.extent,
                               desc.format,
                               VK_IMAGE_USAGE_HOST_TRANSFER_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT,
                               desc.name);
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
        image->transition(*cmd,
                          VK_IMAGE_LAYOUT_GENERAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_HOST_BIT,
                          VK_ACCESS_2_HOST_WRITE_BIT,
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

} // namespace harmonia
