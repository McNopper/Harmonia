#pragma once

#include <volk/volk.h>

#include <array>

#include "harmonia/app/IRenderer.hpp"

namespace harmonia {

/// The default Harmonia renderer: clears the frame to a flat green.
///
/// This exists so Harmonia is independently runnable and so the shared
/// framework (Vulkan context/device, swapchain HDR/SDR negotiation,
/// presentation) can be validated without a real renderer. Hyperion and Theia
/// replace it with their own IRenderer.
class GreenScreenRenderer final : public IRenderer {
  public:
    void record(VkCommandBuffer cmd, const RenderTarget& target) noexcept override;
    void onResize(VkExtent2D extent) noexcept override;
    [[nodiscard]] const char* name() const noexcept override { return "GreenScreenRenderer"; }

  private:
    // Linear-ish green; interpreted in the swapchain's color space.
    std::array<float, 4> m_clearColor{0.0F, 1.0F, 0.0F, 1.0F};
};

} // namespace harmonia
