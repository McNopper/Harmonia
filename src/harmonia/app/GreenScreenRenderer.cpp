#include "harmonia/app/GreenScreenRenderer.hpp"

namespace harmonia {

void GreenScreenRenderer::record(VkCommandBuffer cmd, const RenderTarget& target) noexcept {
    // The host hands us the target image in VK_IMAGE_LAYOUT_GENERAL. A plain
    // clear is all the placeholder renderer needs.
    const VkClearColorValue clear{
        .float32 = {m_clearColor[0], m_clearColor[1], m_clearColor[2], m_clearColor[3]},
    };
    const VkImageSubresourceRange range{
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    vkCmdClearColorImage(cmd, target.image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
}

void GreenScreenRenderer::onResize(VkExtent2D /*extent*/) noexcept {
    // Nothing extent-dependent to recreate.
}

} // namespace harmonia
