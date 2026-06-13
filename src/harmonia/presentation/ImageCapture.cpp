#include "harmonia/presentation/ImageCapture.hpp"

#include <glm/glm.hpp>

#include <cmath>
#include <OpenImageIO/imageio.h>
#include <vector>

#include "harmonia/core/Buffer.hpp"
#include "harmonia/core/Logger.hpp"
#include "harmonia/utils/ColorSpace.hpp"
#include "harmonia/utils/ToneMapping.hpp"

namespace ImageCapture {
namespace {

// Copies the RGBA32F HDR image (in VK_IMAGE_LAYOUT_GENERAL) into a host-visible
// read-back buffer and restores the image to VK_IMAGE_LAYOUT_GENERAL.
[[nodiscard]] std::expected<Buffer, VkResult> readBackHdr(const DeviceContext& ctx,
                                                          const CommandPool& pool,
                                                          const Image& hdrImage) {
    const uint32_t width = hdrImage.extent().width;
    const uint32_t height = hdrImage.extent().height;
    const VkDeviceSize byteSize =
        static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * sizeof(float) * 4U;

    auto readback = Buffer::create(
        ctx, byteSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST, "harmonia.hdr.readback");
    if (!readback) {
        return std::unexpected(readback.error());
    }

    auto cmd = pool.beginOneShot();
    if (!cmd) {
        return std::unexpected(cmd.error());
    }

    hdrImage.transition(*cmd,
                        VK_IMAGE_LAYOUT_GENERAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_ACCESS_2_SHADER_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                        VK_ACCESS_2_TRANSFER_READ_BIT);

    const VkBufferImageCopy copyRegion{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
            VkImageSubresourceLayers{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .imageOffset = VkOffset3D{0, 0, 0},
        .imageExtent = VkExtent3D{width, height, 1},
    };
    vkCmdCopyImageToBuffer(
        *cmd, hdrImage.handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->handle(), 1, &copyRegion);

    hdrImage.transition(*cmd,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                        VK_ACCESS_2_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT);

    if (const VkResult result = pool.endOneShot(*cmd); result != VK_SUCCESS) {
        return std::unexpected(result);
    }
    return readback;
}

} // namespace

bool savePng(const DeviceContext& ctx,
             const CommandPool& pool,
             const Image& hdrImage,
             const std::filesystem::path& path,
             ColorSpace::WorkingColorSpace workingSpace) {
    if (!ctx.isValid() || !hdrImage.isValid()) {
        return false;
    }

    vkDeviceWaitIdle(ctx.device);
    const uint32_t width = hdrImage.extent().width;
    const uint32_t height = hdrImage.extent().height;

    auto readback = readBackHdr(ctx, pool, hdrImage);
    if (!readback) {
        Logger::error("ImageCapture::savePng: read-back failed: VkResult {}", static_cast<int>(readback.error()));
        return false;
    }

    // Tone-map (ACES SDR: working space linear -> Rec.709 linear -> sRGB 8-bit) and
    // pack into a contiguous R8G8B8 byte buffer. The ACES path expects linear
    // Rec.2020 input; a Rec.709 working space is up-converted first (exact).
    const bool upConvert = (workingSpace == ColorSpace::WorkingColorSpace::LinRec709);
    const auto* src = static_cast<const float*>(readback->mappedData());
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 3U);

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t srcIdx = (static_cast<size_t>(y) * width + x) * 4U;
            glm::vec3 hdr(src[srcIdx + 0], src[srcIdx + 1], src[srcIdx + 2]);
            if (upConvert) {
                hdr = ColorSpace::rec709ToRec2020(hdr);
            }
            const glm::vec3 sdrLinear = ToneMapping::acesFittedSDR(hdr);
            const glm::vec3 sdrGamma = ColorSpace::linearRec709ToSrgb(sdrLinear);
            const glm::vec3 clamped = glm::clamp(sdrGamma, 0.f, 1.f);
            const size_t dstIdx = (static_cast<size_t>(y) * width + x) * 3U;
            pixels[dstIdx + 0] = static_cast<uint8_t>(std::lround(clamped.r * 255.f));
            pixels[dstIdx + 1] = static_cast<uint8_t>(std::lround(clamped.g * 255.f));
            pixels[dstIdx + 2] = static_cast<uint8_t>(std::lround(clamped.b * 255.f));
        }
    }

    const int stride = static_cast<int>(width) * 3;
    OIIO::ImageSpec spec(static_cast<int>(width), static_cast<int>(height), 3, OIIO::TypeDesc::UINT8);
    auto out = OIIO::ImageOutput::create(path.string());
    if (!out || !out->open(path.string(), spec)) {
        Logger::error("ImageCapture::savePng: OIIO failed to open '{}': {}", path.string(), OIIO::geterror());
        return false;
    }
    out->write_image(OIIO::TypeDesc::UINT8, pixels.data(), OIIO::AutoStride, static_cast<OIIO::stride_t>(stride));
    out->close();
    Logger::info("Saved tone-mapped PNG to {}", path.string());
    return true;
}

bool saveExr([[maybe_unused]] const DeviceContext& ctx,
             [[maybe_unused]] const CommandPool& pool,
             [[maybe_unused]] const Image& hdrImage,
             const std::filesystem::path& path,
             [[maybe_unused]] ColorSpace::WorkingColorSpace workingSpace) {
    if (!ctx.isValid() || !hdrImage.isValid()) {
        return false;
    }

    vkDeviceWaitIdle(ctx.device);

    auto readback = readBackHdr(ctx, pool, hdrImage);
    if (!readback) {
        Logger::error("ImageCapture::saveExr: read-back failed: VkResult {}", static_cast<int>(readback.error()));
        return false;
    }

    const int w = static_cast<int>(hdrImage.extent().width);
    const int h = static_cast<int>(hdrImage.extent().height);

    // Rec.2020 or Rec.709 primaries + D65 white point, as float[8]: Rx Ry Gx Gy Bx By Wx Wy.
    const float chromaRec2020[8] = {0.708f, 0.292f, 0.170f, 0.797f, 0.131f, 0.046f, 0.3127f, 0.3290f};
    const float chromaRec709[8]  = {0.640f, 0.330f, 0.300f, 0.600f, 0.150f, 0.060f, 0.3127f, 0.3290f};
    const float* chroma = (workingSpace == ColorSpace::WorkingColorSpace::LinRec2020) ? chromaRec2020 : chromaRec709;

    OIIO::ImageSpec spec(w, h, 3, OIIO::TypeDesc::FLOAT);
    spec.attribute("chromaticities", OIIO::TypeDesc(OIIO::TypeDesc::FLOAT, 8), chroma);

    auto out = OIIO::ImageOutput::create(path.string());
    if (!out || !out->open(path.string(), spec)) {
        Logger::error("ImageCapture::saveExr: OIIO failed to open '{}': {}", path.string(), OIIO::geterror());
        return false;
    }

    // Source buffer is RGBA32F; write only RGB (3 channels) with correct x-stride.
    const auto* src = static_cast<const float*>(readback->mappedData());
    const OIIO::stride_t xstride = static_cast<OIIO::stride_t>(4 * sizeof(float));
    out->write_image(OIIO::TypeDesc::FLOAT, src, xstride);
    out->close();
    Logger::info("Saved HDR EXR to {}", path.string());
    return true;
}

} // namespace ImageCapture
