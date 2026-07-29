#ifndef HARMONIA_CORE_BARRIER_HPP
#define HARMONIA_CORE_BARRIER_HPP

#include <volk/volk.h>

#include <span>

namespace harmonia {

/// Builds a color-aspect VkImageMemoryBarrier2 (single mip/layer).
[[nodiscard]] inline VkImageMemoryBarrier2 imageBarrier(VkImage image,
                                                        VkImageLayout oldLayout,
                                                        VkImageLayout newLayout,
                                                        VkPipelineStageFlags2 srcStage,
                                                        VkAccessFlags2 srcAccess,
                                                        VkPipelineStageFlags2 dstStage,
                                                        VkAccessFlags2 dstAccess) noexcept {
    return VkImageMemoryBarrier2{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = srcStage,
        .srcAccessMask = srcAccess,
        .dstStageMask = dstStage,
        .dstAccessMask = dstAccess,
        .oldLayout = oldLayout,
        .newLayout = newLayout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange =
            VkImageSubresourceRange{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
    };
}

/// Records a vkCmdPipelineBarrier2 with the given image barriers.
inline void pipelineBarrier(VkCommandBuffer cmd, std::span<const VkImageMemoryBarrier2> imageBarriers) noexcept {
    const VkDependencyInfo dependencyInfo{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext = nullptr,
        .dependencyFlags = 0,
        .memoryBarrierCount = 0,
        .pMemoryBarriers = nullptr,
        .bufferMemoryBarrierCount = 0,
        .pBufferMemoryBarriers = nullptr,
        .imageMemoryBarrierCount = static_cast<std::uint32_t>(imageBarriers.size()),
        .pImageMemoryBarriers = imageBarriers.data(),
    };
    vkCmdPipelineBarrier2(cmd, &dependencyInfo);
}

} // namespace harmonia
#endif // HARMONIA_CORE_BARRIER_HPP
