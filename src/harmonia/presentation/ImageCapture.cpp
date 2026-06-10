#include "harmonia/presentation/ImageCapture.hpp"

#include <glm/glm.hpp>

#include <cmath>
#include <stb_image_write.h>
#include <vector>

#include "harmonia/core/Buffer.hpp"
#include "harmonia/core/Logger.hpp"
#include "harmonia/utils/ColorSpace.hpp"
#include "harmonia/utils/ToneMapping.hpp"

#ifdef HARMONIA_HAS_OPENEXR
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfChromaticities.h>
#include <OpenEXR/ImfChromaticitiesAttribute.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfOutputFile.h>
#endif

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
    if (!stbi_write_png(
            path.string().c_str(), static_cast<int>(width), static_cast<int>(height), 3, pixels.data(), stride)) {
        Logger::error("ImageCapture::savePng: stbi_write_png failed for {}", path.string());
        return false;
    }
    Logger::info("Saved tone-mapped PNG to {}", path.string());
    return true;
}

bool saveExr([[maybe_unused]] const DeviceContext& ctx,
             [[maybe_unused]] const CommandPool& pool,
             [[maybe_unused]] const Image& hdrImage,
             const std::filesystem::path& path,
             [[maybe_unused]] ColorSpace::WorkingColorSpace workingSpace) {
#ifndef HARMONIA_HAS_OPENEXR
    Logger::warn("OpenEXR support is not enabled; cannot save {}", path.string());
    return false;
#else
    if (!ctx.isValid() || !hdrImage.isValid()) {
        return false;
    }

    vkDeviceWaitIdle(ctx.device);

    auto readback = readBackHdr(ctx, pool, hdrImage);
    if (!readback) {
        Logger::error("ImageCapture::saveExr: read-back failed: VkResult {}", static_cast<int>(readback.error()));
        return false;
    }

    using namespace OPENEXR_IMF_NAMESPACE;
    const int width = static_cast<int>(hdrImage.extent().width);
    const int height = static_cast<int>(hdrImage.extent().height);
    Header header(width, height);
    header.channels().insert("R", Channel(FLOAT));
    header.channels().insert("G", Channel(FLOAT));
    header.channels().insert("B", Channel(FLOAT));

    // Tag the primaries of the working space (linear by definition) so readers
    // do not fall back to the Rec.709 default for Rec.2020 content.
    {
        using V2f = IMATH_NAMESPACE::V2f;
        const Chromaticities chroma =
            (workingSpace == ColorSpace::WorkingColorSpace::LinRec2020)
                ? Chromaticities(V2f(0.708f, 0.292f), V2f(0.170f, 0.797f), V2f(0.131f, 0.046f), V2f(0.3127f, 0.3290f))
                : Chromaticities(V2f(0.640f, 0.330f), V2f(0.300f, 0.600f), V2f(0.150f, 0.060f), V2f(0.3127f, 0.3290f));
        header.insert("chromaticities", ChromaticitiesAttribute(chroma));
    }

    FrameBuffer frameBuffer;
    char* const base = static_cast<char*>(readback->mappedData());
    const size_t pixelStride = sizeof(float) * 4U;
    const size_t rowStride = pixelStride * static_cast<size_t>(width);
    frameBuffer.insert("R", Slice(FLOAT, base + 0U * sizeof(float), pixelStride, rowStride));
    frameBuffer.insert("G", Slice(FLOAT, base + 1U * sizeof(float), pixelStride, rowStride));
    frameBuffer.insert("B", Slice(FLOAT, base + 2U * sizeof(float), pixelStride, rowStride));

    OutputFile file(path.string().c_str(), header);
    file.setFrameBuffer(frameBuffer);
    file.writePixels(height);
    Logger::info("Saved HDR EXR to {}", path.string());
    return true;
#endif
}

} // namespace ImageCapture
