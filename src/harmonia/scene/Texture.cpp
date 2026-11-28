#include "harmonia/scene/Texture.hpp"

#include <volk/volk.h>

#include <OpenImageIO/half.h>
#include <OpenImageIO/imageio.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "harmonia/core/DdsImage.hpp"
#include "harmonia/core/HostImageUpload.hpp"
#include "harmonia/core/Logger.hpp"
#include "harmonia/core/Sampler.hpp"
#include "harmonia/utils/ColorSpace.hpp"

namespace harmonia::scene {

namespace {

using harmonia::utils::ColorSpace::WorkingColorSpace;

/// Every texture is sampled by the raster forward pass, compute passes and the path tracer.
constexpr VkPipelineStageFlags2 kSamplingStages = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                                  VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
constexpr VkAccessFlags2 kSamplingAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;

/// The scene sampler shared by every texture: REPEAT x3 with anisotropy.
[[nodiscard]] core::SamplerSpec sceneSamplerSpec(const DeviceContext& ctx) noexcept {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(ctx.physicalDevice, &props);
    return core::SamplerSpec{
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .anisotropyEnable = VK_TRUE,
        .maxAnisotropy = std::min(16.0f, props.limits.maxSamplerAnisotropy),
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
    };
}

/// Colour data whose primaries equal the working space needs no primaries conversion.
[[nodiscard]] constexpr bool samePrimaries(TextureColorSpace colorSpace, WorkingColorSpace workingSpace) noexcept {
    return (colorSpace == TextureColorSpace::LinRec2020Scene && workingSpace == WorkingColorSpace::LinRec2020) ||
           (colorSpace == TextureColorSpace::LinRec709Scene && workingSpace == WorkingColorSpace::LinRec709) ||
           (colorSpace == TextureColorSpace::LinAcesCgScene && workingSpace == WorkingColorSpace::LinAcesCg);
}

/// A texture needs a colour conversion when it is colour data whose encoding or primaries
/// differ from the (linear) working colour space.
[[nodiscard]] constexpr bool needsColorConversion(TextureColorSpace colorSpace,
                                                  WorkingColorSpace workingSpace) noexcept {
    return colorSpace != TextureColorSpace::Data && !samePrimaries(colorSpace, workingSpace);
}

/// Applies the declared transfer-function decode and primaries conversion to one RGB color.
[[nodiscard]] sm::float3
toWorkingColor(sm::float3 color, TextureColorSpace colorSpace, WorkingColorSpace workingSpace) noexcept {
    switch (colorSpace) {
    case TextureColorSpace::SrgbRec709Scene:
        color = harmonia::utils::ColorSpace::srgbToLinearRec709(color);
        return harmonia::utils::ColorSpace::convertLinearRgb(color, WorkingColorSpace::LinRec709, workingSpace);
    case TextureColorSpace::LinRec709Scene:
        return harmonia::utils::ColorSpace::convertLinearRgb(color, WorkingColorSpace::LinRec709, workingSpace);
    case TextureColorSpace::LinRec2020Scene:
        return harmonia::utils::ColorSpace::convertLinearRgb(color, WorkingColorSpace::LinRec2020, workingSpace);
    case TextureColorSpace::LinAcesCgScene:
        return harmonia::utils::ColorSpace::convertLinearRgb(color, WorkingColorSpace::LinAcesCg, workingSpace);
    case TextureColorSpace::Data:
        return color;
    }
    return color;
}

/// The DDS path is selected by the file extension, case-insensitively.
[[nodiscard]] bool isDdsPath(const std::filesystem::path& path) noexcept {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return extension == ".dds";
}

} // namespace

/// Adopts an uploaded image as a Texture and names the sampler.
Texture Texture::adoptUpload(core::HostImageUpload&& uploaded, const DeviceContext& ctx, const std::string& name) {
    Texture texture;
    texture.m_image = std::move(uploaded.image);
    texture.m_width = texture.m_image.extent().width;
    texture.m_height = texture.m_image.extent().height;
    texture.m_mipLevels = texture.m_image.mipLevels(); // PERF8: full chain when blit-capable
    texture.m_sampler = std::move(uploaded.sampler);

    if (!name.empty()) {
        ctx.setDebugName(VK_OBJECT_TYPE_SAMPLER, texture.m_sampler.get(), std::string(name).append(".sampler").c_str());
    }
    return texture;
}

namespace {

/// A block-compressed DDS: the blocks are uploaded as they are, one host copy per level,
/// and never decoded. Nothing can be converted afterwards, so every colour conversion the
/// PNG/TIFF path does on the CPU is rejected instead. The one conversion that a compressed
/// format can represent is the sRGB decode, done by the hardware through the *_SRGB
/// variant of the format, and only when the scene asks for it.
[[nodiscard]] std::expected<core::HostImageUpload, VkResult> loadDds(const DeviceContext& ctx,
                                                                     const core::CommandPool& cmdPool,
                                                                     const std::filesystem::path& path,
                                                                     const std::string& pathStr,
                                                                     const std::string& name,
                                                                     TextureColorSpace colorSpace,
                                                                     WorkingColorSpace workingSpace) {
    auto dds = core::DdsImage::loadFromFile(path);
    if (!dds) {
        core::Logger::error(
            "Texture::loadFromFile: DDS '{}' rejected: {}", pathStr, core::ddsErrorMessage(dds.error()));
        return std::unexpected(dds.error() == core::DdsError::UnsupportedFormat ? VK_ERROR_FORMAT_NOT_SUPPORTED
                                                                                : VK_ERROR_INITIALIZATION_FAILED);
    }

    const bool srgbDecode = (colorSpace == TextureColorSpace::SrgbRec709Scene);
    const bool needsPrimariesConversion =
        colorSpace != TextureColorSpace::Data &&
        !((colorSpace == TextureColorSpace::SrgbRec709Scene || colorSpace == TextureColorSpace::LinRec709Scene
               ? workingSpace == WorkingColorSpace::LinRec709
               : false) ||
          (colorSpace == TextureColorSpace::LinRec2020Scene && workingSpace == WorkingColorSpace::LinRec2020) ||
          (colorSpace == TextureColorSpace::LinAcesCgScene && workingSpace == WorkingColorSpace::LinAcesCg));
    if (needsPrimariesConversion) {
        core::Logger::error("Texture::loadFromFile: DDS '{}' is colour data that needs a conversion to the working "
                            "colour space; a block-compressed texture cannot represent it",
                            pathStr);
        return std::unexpected(VK_ERROR_FORMAT_NOT_SUPPORTED);
    }
    if (srgbDecode && !dds->hasSrgbVariant()) {
        core::Logger::error("Texture::loadFromFile: DDS '{}' is declared sRGB but its format has no sRGB variant",
                            pathStr);
        return std::unexpected(VK_ERROR_FORMAT_NOT_SUPPORTED);
    }
    if (dds->fileIsSrgb() && !srgbDecode) {
        core::Logger::error(
            "Texture::loadFromFile: DDS '{}' is stored as sRGB but the scene declares it data or linear", pathStr);
        return std::unexpected(VK_ERROR_FORMAT_NOT_SUPPORTED);
    }

    std::vector<core::HostImageMipLevel> mips;
    mips.reserve(dds->levels().size());
    for (const core::DdsImage::Level& level : dds->levels()) {
        const std::span<const std::byte> bytes = dds->levelBytes(level);
        mips.push_back(core::HostImageMipLevel{
            .data = bytes.data(),
            .byteLength = bytes.size(),
            .width = level.width,
            .height = level.height,
        });
    }

    auto uploaded = core::uploadHostImage(ctx,
                                          cmdPool,
                                          core::HostImageUploadDesc{
                                              .format = dds->format(srgbDecode),
                                              .pixels = nullptr,
                                              .extent = VkExtent2D{dds->width(), dds->height()},
                                              .finalStages = kSamplingStages,
                                              .finalAccess = kSamplingAccess,
                                              .name = name,
                                              .sampler = sceneSamplerSpec(ctx),
                                              .mips = mips,
                                          });
    if (!uploaded) {
        core::Logger::error("Texture::loadFromFile: upload of DDS '{}' failed: VkResult {}",
                            pathStr,
                            static_cast<int>(uploaded.error()));
        return std::unexpected(uploaded.error());
    }
    return uploaded;
}

} // namespace

std::expected<Texture, VkResult> Texture::create(const DeviceContext& ctx,
                                                 const core::CommandPool& cmdPool,
                                                 std::span<const std::byte> pixels,
                                                 std::uint32_t width,
                                                 std::uint32_t height,
                                                 std::string_view name) {
    return createWithFormat(ctx, cmdPool, pixels, width, height, VK_FORMAT_R8G8B8A8_UNORM, name);
}

std::expected<Texture, VkResult> Texture::createWithFormat(const DeviceContext& ctx,
                                                           const core::CommandPool& cmdPool,
                                                           std::span<const std::byte> pixels,
                                                           std::uint32_t width,
                                                           std::uint32_t height,
                                                           VkFormat format,
                                                           std::string_view name) {
    if (width == 0 || height == 0 || pixels.empty()) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    // Vulkan 1.4 hostImageCopy: stream host pixels straight into the (optimal-tiling) image
    // with vkCopyMemoryToImage - no staging buffer, no device-side copy (shared helper).
    // Layout path: UNDEFINED -> GENERAL (queue), host copy into GENERAL,
    // GENERAL -> SHADER_READ_ONLY (queue). Sampler is REPEAT x3 with anisotropy.
    auto uploaded = core::uploadHostImage(ctx,
                                          cmdPool,
                                          core::HostImageUploadDesc{
                                              .format = format,
                                              .pixels = pixels.data(),
                                              .extent = VkExtent2D{width, height},
                                              .finalStages = kSamplingStages,
                                              .finalAccess = kSamplingAccess,
                                              .name = name,
                                              .sampler = sceneSamplerSpec(ctx),
                                          });
    if (!uploaded) {
        return std::unexpected(uploaded.error());
    }

    return Texture::adoptUpload(std::move(*uploaded), ctx, std::string(name));
}

std::expected<Texture, VkResult> Texture::loadFromFile(const DeviceContext& ctx,
                                                       const core::CommandPool& cmdPool,
                                                       const std::filesystem::path& path,
                                                       TextureColorSpace colorSpace,
                                                       harmonia::utils::ColorSpace::WorkingColorSpace workingSpace,
                                                       std::string_view name) {
    const std::string pathStr = path.string();
    const std::string textureName = name.empty() ? path.filename().string() : std::string(name);

    // DDS is block-compressed: its blocks are uploaded undecoded (no OIIO).
    if (isDdsPath(path)) {
        auto uploaded = loadDds(ctx, cmdPool, path, pathStr, textureName, colorSpace, workingSpace);
        if (!uploaded) {
            return std::unexpected(uploaded.error());
        }
        return Texture::adoptUpload(std::move(*uploaded), ctx, textureName);
    }

    // Disable OIIO automatic color management — Harmonia handles all color
    // conversions explicitly. Without this, OIIO may apply an OCIO transform
    // if a color config is active (e.g. sRGB linearisation on PNG, or
    // chromaticity adaptation on EXR), corrupting our own primaries conversion.
    OIIO::ImageSpec openConfig;
    openConfig.attribute("raw_color", 1);

    auto inp = OIIO::ImageInput::open(pathStr, &openConfig);
    if (!inp) {
        core::Logger::error("Texture::loadFromFile: OIIO failed to open '{}': {}", pathStr, OIIO::geterror());
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }
    const OIIO::ImageSpec& spec = inp->spec();
    const std::size_t w = static_cast<std::size_t>(spec.width);
    const std::size_t h = static_cast<std::size_t>(spec.height);
    const auto pixelCount = w * h;

    const std::int32_t nchans = std::min(spec.nchannels, 4);
    const bool needsConversion = needsColorConversion(colorSpace, workingSpace);
    const auto sourceType = static_cast<OIIO::TypeDesc::BASETYPE>(spec.format.basetype);

    // An unconverted 16-bit integer source stays integer all the way to the GPU. This is
    // especially important for data maps: quantizing them to 8 bits would collapse adjacent
    // height/roughness/coat values before sampling.
    if (sourceType == OIIO::TypeDesc::UINT16 && !needsConversion) {
        std::vector<std::uint16_t> raw(pixelCount * 4, 65535U);
        if (!inp->read_image(0,
                             0,
                             0,
                             nchans,
                             OIIO::TypeDesc::UINT16,
                             raw.data(),
                             static_cast<OIIO::stride_t>(4 * sizeof(std::uint16_t)))) {
            core::Logger::error(
                "Texture::loadFromFile: OIIO 16-bit read_image failed for '{}': {}", pathStr, inp->geterror());
            return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
        }
        inp->close();
        const auto bytes = std::as_bytes(std::span<const std::uint16_t>(raw));
        return createWithFormat(ctx,
                                cmdPool,
                                bytes,
                                static_cast<std::uint32_t>(w),
                                static_cast<std::uint32_t>(h),
                                VK_FORMAT_R16G16B16A16_UNORM,
                                textureName);
    }

    // Half/float sources, and 16-bit colors that need a transfer-function or primaries
    // conversion, use floating point. No 8-bit intermediate is involved. Half has 11 bits of
    // significand; values outside its finite range are rejected rather than clamped.
    if (sourceType == OIIO::TypeDesc::UINT16 || sourceType == OIIO::TypeDesc::HALF ||
        sourceType == OIIO::TypeDesc::FLOAT) {
        std::vector<float> raw(pixelCount * 4, 1.0F);
        if (!inp->read_image(
                0, 0, 0, nchans, OIIO::TypeDesc::FLOAT, raw.data(), static_cast<OIIO::stride_t>(4 * sizeof(float)))) {
            core::Logger::error(
                "Texture::loadFromFile: OIIO floating-point read_image failed for '{}': {}", pathStr, inp->geterror());
            return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
        }
        inp->close();

        std::vector<std::uint16_t> converted(pixelCount * 4);
        for (std::size_t i = 0; i < pixelCount; ++i) {
            sm::float3 linear{raw[i * 4 + 0], raw[i * 4 + 1], raw[i * 4 + 2]};
            if (needsConversion) {
                linear = toWorkingColor(linear, colorSpace, workingSpace);
            }
            const std::array<float, 4> channels{linear.r, linear.g, linear.b, raw[i * 4 + 3]};
            for (std::size_t channel = 0; channel < channels.size(); ++channel) {
                const float value = channels[channel];
                if (!std::isfinite(value) || std::abs(value) > 65504.0F) {
                    core::Logger::error(
                        "Texture::loadFromFile: '{}' has channel value {} outside finite half range", pathStr, value);
                    return std::unexpected(VK_ERROR_FORMAT_NOT_SUPPORTED);
                }
                converted[i * 4 + channel] = Imath::half(value).bits();
            }
        }
        const auto bytes = std::as_bytes(std::span<const std::uint16_t>(converted));
        return createWithFormat(ctx,
                                cmdPool,
                                bytes,
                                static_cast<std::uint32_t>(w),
                                static_cast<std::uint32_t>(h),
                                VK_FORMAT_R16G16B16A16_SFLOAT,
                                textureName);
    }

    // Pre-fill with 0xFF so any missing channels default to opaque/white.
    // OIIO fills channels beyond spec.nchannels with 0, which would make
    // RGB-only images fully transparent (alpha=0) on the GPU.
    std::vector<std::uint8_t> raw(pixelCount * 4, 0xFF);
    // xstride=4: always advance 4 bytes per pixel in our RGBA buffer so the
    // pre-filled alpha byte is not overwritten for 3-channel source images.
    if (!inp->read_image(0, 0, 0, nchans, OIIO::TypeDesc::UINT8, raw.data(), static_cast<OIIO::stride_t>(4))) {
        core::Logger::error("Texture::loadFromFile: OIIO read_image failed for '{}': {}", pathStr, inp->geterror());
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }
    inp->close();

    std::vector<std::uint8_t> converted(pixelCount * 4);

    if (!needsConversion) {
        // Data maps (normal/ORM/roughness) or already in working space — copy verbatim.
        std::memcpy(converted.data(), raw.data(), pixelCount * 4);
    } else {
        // Convert each pixel: decode transfer function, then primaries → working space.
        for (std::size_t i = 0; i < pixelCount; ++i) {
            const float r = static_cast<float>(raw[i * 4 + 0]) / 255.0f;
            const float g = static_cast<float>(raw[i * 4 + 1]) / 255.0f;
            const float b = static_cast<float>(raw[i * 4 + 2]) / 255.0f;
            const std::uint8_t a = raw[i * 4 + 3];

            const sm::float3 linear = toWorkingColor(sm::float3{r, g, b}, colorSpace, workingSpace);
            converted[i * 4 + 0] = static_cast<std::uint8_t>(std::lround(sm::saturate(linear.r) * 255.0f));
            converted[i * 4 + 1] = static_cast<std::uint8_t>(std::lround(sm::saturate(linear.g) * 255.0f));
            converted[i * 4 + 2] = static_cast<std::uint8_t>(std::lround(sm::saturate(linear.b) * 255.0f));
            converted[i * 4 + 3] = a;
        }
    }

    const auto bytes = std::as_bytes(std::span<const std::uint8_t>(converted));
    return create(ctx, cmdPool, bytes, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), textureName);
}

} // namespace harmonia::scene
