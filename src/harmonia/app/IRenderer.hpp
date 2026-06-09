#pragma once

#include <volk/volk.h>

#include "harmonia/presentation/OutputColorSpace.hpp"

namespace harmonia {

/// Per-frame target a renderer draws into, supplied by the host (App) each
/// frame. For now this is the swapchain image directly; once the full HDR →
/// tone-map presentation path is wired the renderers will instead fill an HDR
/// target the host tone-maps. The renderer seam stays the same either way.
struct RenderTarget {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkExtent2D extent = {};
    OutputColorSpace colorSpace = OutputColorSpace::eSDR;
};

/// Minimal renderer interface. Harmonia ships a trivial green-screen default;
/// Hyperion (offline path tracer) and Theia (real-time forward renderer) plug
/// in their own implementation, driven by Harmonia's Vulkan context/device,
/// swapchain and presentation.
class IRenderer {
  public:
    IRenderer() = default;
    IRenderer(const IRenderer&) = delete;
    IRenderer& operator=(const IRenderer&) = delete;
    IRenderer(IRenderer&&) = delete;
    IRenderer& operator=(IRenderer&&) = delete;
    virtual ~IRenderer() = default;

    /// Record this frame's draw commands into @p cmd. The target image is in
    /// VK_IMAGE_LAYOUT_GENERAL; the host transitions it to PRESENT afterwards.
    virtual void record(VkCommandBuffer cmd, const RenderTarget& target) noexcept = 0;

    /// Called when the render target is resized.
    virtual void onResize(VkExtent2D extent) noexcept = 0;

    /// Debug name for logging.
    [[nodiscard]] virtual const char* name() const noexcept = 0;
};

} // namespace harmonia
