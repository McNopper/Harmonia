#ifndef HARMONIA_APP_OFFSCREENCAPTURE_HPP
#define HARMONIA_APP_OFFSCREENCAPTURE_HPP

#include <volk/volk.h>

#include <cstdint>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/CommandPool.hpp"
#include "harmonia/core/Image.hpp"
#include "harmonia/presentation/OutputColorSpace.hpp"
#include "harmonia/presentation/ToneMapper.hpp"
#include "harmonia/utils/ColorSpace.hpp"

namespace harmonia {

/// Offscreen (headless --output) capture resources extracted from App (R8/CH9): a fixed
/// 8-bit sRGB capture image and a dedicated SDR ToneMapper pipeline used to produce a
/// tone-mapped PNG whose tone curve matches the interactive window. The frame-loop
/// orchestration and EXR save stay in App.
class OffscreenCapture {
  public:
    /// Run the same tone mapper the window uses into the capture image (lazily creating the
    /// capture ToneMapper + capture Image). Returns false if the capture pipeline/image
    /// cannot be created (caller falls back to CPU tonemap). The caller gates this on the
    /// display tone mapper being available.
    [[nodiscard]] bool tonemapToCaptureImage(const DeviceContext& ctx,
                                             const CommandPool& cmdPool,
                                             VkExtent2D extent,
                                             const Image& sceneOutput,
                                             VkPipelineStageFlags2 srcStage,
                                             VkAccessFlags2 srcAccess,
                                             std::uint32_t tonemapper,
                                             ColorSpace::WorkingColorSpace workingSpace);
    [[nodiscard]] const Image& captureImage() const noexcept { return m_displayCaptureImage; }

  private:
    ToneMapper m_captureToneMapper;
    Image m_displayCaptureImage;
};

} // namespace harmonia

#endif // HARMONIA_APP_OFFSCREENCAPTURE_HPP
