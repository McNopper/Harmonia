#ifndef HARMONIA_APP_IRENDERER_HPP
#define HARMONIA_APP_IRENDERER_HPP

#include <volk/volk.h>

#include "harmonia/presentation/OutputColorSpace.hpp"
#include "harmonia/utils/ColorSpace.hpp"

namespace harmonia {

/// Per-frame target a renderer draws into, supplied by the host (App) each
/// frame.
///
/// Scene-referred renderers (Hyperion's path tracer, Theia's forward stack)
/// receive the host's linear HDR image: all values are linear radiometric
/// quantities in @ref workingColorSpace. The renderer must leave the image in
/// VK_IMAGE_LAYOUT_GENERAL; the host then runs the shared ToneMapper — the
/// glue between the scene-referred and display-referred states — and presents.
///
/// Display-referred renderers (future post-tonemap stage) will instead receive
/// the tonemapped target, where @ref colorSpace describes the output encoding.
struct RenderTarget {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkExtent2D extent = {};
    /// Display-referred output encoding (meaningful post-tonemap).
    OutputColorSpace colorSpace = OutputColorSpace::eSDR;
    /// Scene-referred working color space (always linear; meaningful pre-tonemap).
    ColorSpace::WorkingColorSpace workingColorSpace = ColorSpace::WorkingColorSpace::LinRec2020;
};

/// Minimal renderer interface. Harmonia ships a trivial green-screen default;
/// Hyperion (offline path tracer) and Theia (real-time forward renderer) plug
/// in their own implementation, driven by Harmonia's Vulkan context/device,
/// swapchain, tone mapping and presentation (see harmonia::App).
class IRenderer {
  public:
    IRenderer() = default;
    IRenderer(const IRenderer&) = delete;
    IRenderer& operator=(const IRenderer&) = delete;
    IRenderer(IRenderer&&) = delete;
    IRenderer& operator=(IRenderer&&) = delete;
    virtual ~IRenderer() = default;

    /// Record this frame's draw commands into @p cmd. The renderer owns the
    /// target's layout transitions within its pass (first use may be
    /// VK_IMAGE_LAYOUT_UNDEFINED) and must leave the image in
    /// VK_IMAGE_LAYOUT_GENERAL for the host's tonemap pass.
    virtual void record(VkCommandBuffer cmd, const RenderTarget& target) noexcept = 0;

    /// Pipeline stage(s) that produce the renderer's final write into the
    /// target — used by the host for the pre-tonemap barrier.
    [[nodiscard]] virtual VkPipelineStageFlags2 outputStageMask() const noexcept {
        return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }

    /// Access mask(s) of the renderer's final write into the target — paired
    /// with outputStageMask() by the host's pre-tonemap barrier.
    [[nodiscard]] virtual VkAccessFlags2 outputAccessMask() const noexcept { return VK_ACCESS_2_SHADER_WRITE_BIT; }

    /// Optional renderer-owned G-buffers surfaced to Harmonia's shared stages.
    /// Default: unavailable.
    [[nodiscard]] virtual VkImageView gNormalView() const noexcept { return VK_NULL_HANDLE; }
    [[nodiscard]] virtual VkImageView gDepthView() const noexcept { return VK_NULL_HANDLE; }
    /// Per-pixel (dx, dy) motion-vector image view in VK_IMAGE_LAYOUT_GENERAL
    /// (R32G32_SFLOAT). Populated by renderers that support camera/object motion
    /// vectors; defaults to null (denoiser falls back to static history lookup).
    [[nodiscard]] virtual VkImageView motionVectorView() const noexcept { return VK_NULL_HANDLE; }

    /// Called when the render target is resized.
    virtual void onResize(VkExtent2D extent) noexcept = 0;

    /// Debug name for logging.
    [[nodiscard]] virtual const char* name() const noexcept = 0;
};

} // namespace harmonia
#endif // HARMONIA_APP_IRENDERER_HPP
