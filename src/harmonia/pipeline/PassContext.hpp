#ifndef HARMONIA_PIPELINE_PASSCONTEXT_HPP
#define HARMONIA_PIPELINE_PASSCONTEXT_HPP

#include <volk/volk.h>

#include <cstdint>

#include "harmonia/utils/ColorSpace.hpp"
#include "harmonia/utils/OutputColorSpace.hpp"

namespace harmonia {

class Image; // forward — avoids pulling in the full header

/// Per-frame context passed to every IRenderPass::record() call.
/// Each pass reads only the fields it needs.
struct PassContext {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    std::uint32_t frameIndex = 0;
    std::uint32_t frameSampleIndex = 0;
    std::uint32_t rngSeed = 0x12345678U;
    bool deterministicReplay = false;
    VkExtent2D extent = {};
    bool fixedView = false;                    ///< true for deterministic offscreen accumulation mode
    std::uint64_t accumulationResetToken = 0U; ///< explicit reset key for progressive accumulation history
    /// Separate reset key for temporal denoiser history. Changes only on scene/resize/config
    /// changes — NOT on camera movement. This allows the A-SVGF denoiser to reproject its
    /// temporal history across camera motion via motion vectors (SVGF/A-SVGF design intent),
    /// while AccumulationPass still resets its per-view average on every camera movement.
    std::uint64_t denoiserResetToken = 0U;

    /// Outputs from PathTracer — all in VK_IMAGE_LAYOUT_GENERAL.
    const Image* hdrBuffer = nullptr; ///< accumulated HDR radiance
    VkImageView gNormalView = VK_NULL_HANDLE;
    VkImageView gDepthView = VK_NULL_HANDLE;
    VkImageView transparentNormalView = VK_NULL_HANDLE; ///< D0: first transparent-interface normal
    VkImageView transparentDepthView = VK_NULL_HANDLE;  ///< D0: first transparent-interface depth
    VkImageView motionVectorView = VK_NULL_HANDLE;      ///< D0 future: interactive temporal reprojection

    /// Denoiser output (null if denoiser was not run; ToneMapper falls back to hdrBuffer).
    const Image* denoised = nullptr;

    /// Swapchain image view for the current frame (only needed by ToneMapper).
    VkImageView swapchainView = VK_NULL_HANDLE;
    OutputColorSpace colorSpace = OutputColorSpace::eSDR;
    std::uint32_t tonemapper = 0;
    harmonia::ColorSpace::WorkingColorSpace workingColorSpace = harmonia::ColorSpace::WorkingColorSpace::LinRec2020;
};

inline constexpr std::uint32_t kComputeGroupSize = 8U;

[[nodiscard]] constexpr std::uint32_t dispatchGroups1D(std::uint32_t extent,
                                                       std::uint32_t groupSize = kComputeGroupSize) noexcept {
    return (extent + (groupSize - 1U)) / groupSize;
}

} // namespace harmonia

#endif // HARMONIA_PIPELINE_PASSCONTEXT_HPP
