#include "harmonia/app/OffscreenCapture.hpp"

#include <volk/volk.h>

#include <array>
#include <filesystem>

#include "harmonia/core/Barrier.hpp"
#include "harmonia/core/Logger.hpp"

namespace harmonia {

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif

bool OffscreenCapture::tonemapToCaptureImage(const DeviceContext& ctx,
                                             const CommandPool& cmdPool,
                                             VkExtent2D extent,
                                             const Image& sceneOutput,
                                             VkPipelineStageFlags2 srcStage,
                                             VkAccessFlags2 srcAccess,
                                             std::uint32_t tonemapper,
                                             ColorSpace::WorkingColorSpace workingSpace) {
    // A PNG is inherently SDR, so we cannot bit-match an HDR10 / scRGB swapchain. Instead we
    // run the SAME tone mapper the window uses, but into a fixed 8-bit sRGB target
    // (OutputColorSpace::eSDR). The tone curve matches the window; only the container differs.
    constexpr VkFormat kCaptureFormat = VK_FORMAT_R8G8B8A8_UNORM; // RGBA order -> no channel swap

    if (m_captureToneMapper.isValid() == false) {
        const std::filesystem::path shaderDir = HARMONIA_SHADER_DIR;
        auto toneMapper = ToneMapper::create(
            ctx, kCaptureFormat, shaderDir / "tonemap_vert.spv", shaderDir / "tonemap.spv");
        if (!toneMapper) {
            Logger::error("Capture tone mapper creation failed: VkResult {}", static_cast<int>(toneMapper.error()));
            return false;
        }
        m_captureToneMapper = std::move(*toneMapper);
    }

    auto img = Image::create(ctx,
                             extent,
                             kCaptureFormat,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                 VK_IMAGE_USAGE_SAMPLED_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT,
                             "harmonia.displayCapture");
    if (!img) {
        Logger::error("Display capture image creation failed: VkResult {}", static_cast<int>(img.error()));
        return false;
    }
    m_displayCaptureImage = std::move(*img);

    auto cmd = cmdPool.beginOneShot();
    if (!cmd) {
        return false;
    }

    const std::array preBarriers{
        imageBarrier(sceneOutput.handle(),
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     srcStage,
                     srcAccess,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT),
        imageBarrier(m_displayCaptureImage.handle(),
                     VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_NONE,
                     0,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT),
    };
    pipelineBarrier(*cmd, preBarriers);

    m_captureToneMapper.record(*cmd,
                               sceneOutput.view(),
                               m_displayCaptureImage.view(),
                               extent,
                               OutputColorSpace::eSDR,
                               tonemapper,
                               workingSpace);

    const std::array postBarriers{
        imageBarrier(m_displayCaptureImage.handle(),
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT),
    };
    pipelineBarrier(*cmd, postBarriers);

    return cmdPool.endOneShot(*cmd) == VK_SUCCESS;
}

#ifdef __clang__
#pragma clang diagnostic pop
#endif

} // namespace harmonia
