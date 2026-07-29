#include "harmonia/scene/Texture.hpp"

#include <volk/volk.h>

#include <OpenImageIO/imageio.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vma/vk_mem_alloc.h>

#include "harmonia/core/Buffer.hpp"
#include "harmonia/core/Logger.hpp"
#include "harmonia/core/Sampler.hpp"
#include "harmonia/utils/ColorSpace.hpp"

namespace {
[[nodiscard]] std::expected<Buffer, VkResult>
createStagingBuffer(const DeviceContext& ctx, std::span<const std::byte> pixels, std::string_view name) {
    auto staging = Buffer::create(ctx,
                                  static_cast<VkDeviceSize>(pixels.size()),
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  std::string(name).append(".staging"));
    if (!staging) {
        return std::unexpected(staging.error());
    }

    staging->uploadData(pixels.data(), pixels.size(), 0);
    return std::move(*staging);
}
} // namespace

Texture::~Texture() noexcept {
    reset();
}

Texture::Texture(Texture&& other) noexcept
    : m_ctx(other.m_ctx),
      m_image(std::move(other.m_image)),
      m_sampler(std::exchange(other.m_sampler, VK_NULL_HANDLE)),
      m_width(std::exchange(other.m_width, 0)),
      m_height(std::exchange(other.m_height, 0)),
      m_mipLevels(std::exchange(other.m_mipLevels, 1)) {
    other.m_ctx = nullptr;
}

Texture& Texture::operator=(Texture&& other) noexcept {
    if (this != &other) {
        reset();
        m_ctx = other.m_ctx;
        m_image = std::move(other.m_image);
        m_sampler = std::exchange(other.m_sampler, VK_NULL_HANDLE);
        m_width = std::exchange(other.m_width, 0);
        m_height = std::exchange(other.m_height, 0);
        m_mipLevels = std::exchange(other.m_mipLevels, 1);
        other.m_ctx = nullptr;
    }
    return *this;
}

std::expected<Texture, VkResult> Texture::create(const DeviceContext& ctx,
                                                 const CommandPool& cmdPool,
                                                 std::span<const std::byte> pixels,
                                                 std::uint32_t width,
                                                 std::uint32_t height,
                                                 std::string_view name) {
    if (width == 0 || height == 0 || pixels.empty()) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    constexpr VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    auto image = Image::create(ctx,
                               VkExtent2D{width, height},
                               format,
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT,
                               name);
    if (!image) {
        return std::unexpected(image.error());
    }

    auto staging = createStagingBuffer(ctx, pixels, name);
    if (!staging) {
        return std::unexpected(staging.error());
    }

    auto cmd = cmdPool.beginOneShot();
    if (!cmd) {
        return std::unexpected(cmd.error());
    }

    image->transition(*cmd,
                      VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_2_NONE,
                      0,
                      VK_PIPELINE_STAGE_2_COPY_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT);

    const VkBufferImageCopy region{
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
    vkCmdCopyBufferToImage(*cmd, staging->handle(), image->handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    image->transition(*cmd,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COPY_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                          VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    if (const VkResult submitResult = cmdPool.endOneShot(*cmd); submitResult != VK_SUCCESS) {
        return std::unexpected(submitResult);
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(ctx.physicalDevice, &props);

    const VkSamplerCreateInfo samplerInfo = harmonia::makeSamplerCreateInfo({
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .anisotropyEnable = VK_TRUE,
        .maxAnisotropy = std::min(16.0f, props.limits.maxSamplerAnisotropy),
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
    });

    Texture texture;
    texture.m_ctx = &ctx;
    texture.m_image = std::move(*image);
    texture.m_width = width;
    texture.m_height = height;

    if (const VkResult result = vkCreateSampler(ctx.device, &samplerInfo, nullptr, &texture.m_sampler);
        result != VK_SUCCESS) {
        return std::unexpected(result);
    }

    if (!name.empty()) {
        ctx.setDebugName(VK_OBJECT_TYPE_SAMPLER,
                         reinterpret_cast<std::uint64_t>(texture.m_sampler),
                         std::string(name).append(".sampler").c_str());
    }

    return texture;
}

std::expected<Texture, VkResult> Texture::loadFromFile(const DeviceContext& ctx,
                                                       const CommandPool& cmdPool,
                                                       const std::filesystem::path& path,
                                                       TextureColorSpace colorSpace,
                                                       ColorSpace::WorkingColorSpace workingSpace,
                                                       std::string_view name) {
    const std::string pathStr = path.string();

    // Disable OIIO automatic color management — Harmonia handles all color
    // conversions explicitly. Without this, OIIO may apply an OCIO transform
    // if a color config is active (e.g. sRGB linearisation on PNG, or
    // chromaticity adaptation on EXR), corrupting our own primaries conversion.
    OIIO::ImageSpec openConfig;
    openConfig.attribute("raw_color", 1);

    auto inp = OIIO::ImageInput::open(pathStr, &openConfig);
    if (!inp) {
        Logger::error("Texture::loadFromFile: OIIO failed to open '{}': {}", pathStr, OIIO::geterror());
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }
    const OIIO::ImageSpec& spec = inp->spec();
    const std::size_t w = static_cast<std::size_t>(spec.width);
    const std::size_t h = static_cast<std::size_t>(spec.height);
    const auto pixelCount = w * h;

    // Pre-fill with 0xFF so any missing channels default to opaque/white.
    // OIIO fills channels beyond spec.nchannels with 0, which would make
    // RGB-only images fully transparent (alpha=0) on the GPU.
    std::vector<std::uint8_t> raw(pixelCount * 4, 0xFF);
    const std::int32_t nchans = std::min(spec.nchannels, 4);
    // xstride=4: always advance 4 bytes per pixel in our RGBA buffer so the
    // pre-filled alpha byte is not overwritten for 3-channel source images.
    if (!inp->read_image(0, 0, 0, nchans, OIIO::TypeDesc::UINT8, raw.data(), static_cast<OIIO::stride_t>(4))) {
        Logger::error("Texture::loadFromFile: OIIO read_image failed for '{}': {}", pathStr, inp->geterror());
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }
    inp->close();

    std::vector<std::uint8_t> converted(pixelCount * 4);

    // A texture needs CPU conversion when it is color data whose encoding or
    // primaries differ from the (linear) working color space.
    const bool sameSpace =
        (colorSpace == TextureColorSpace::LinRec2020Scene &&
         workingSpace == ColorSpace::WorkingColorSpace::LinRec2020) ||
        (colorSpace == TextureColorSpace::LinRec709Scene && workingSpace == ColorSpace::WorkingColorSpace::LinRec709);
    const bool needsConversion = (colorSpace != TextureColorSpace::Data && !sameSpace);

    if (!needsConversion) {
        // Data maps (normal/ORM/roughness) or already in working space — copy verbatim.
        std::memcpy(converted.data(), raw.data(), pixelCount * 4);
    } else {
        const bool toRec2020 = (workingSpace == ColorSpace::WorkingColorSpace::LinRec2020);
        // Convert each pixel: decode transfer function, then primaries → working space.
        for (std::size_t i = 0; i < pixelCount; ++i) {
            const float r = static_cast<float>(raw[i * 4 + 0]) / 255.0f;
            const float g = static_cast<float>(raw[i * 4 + 1]) / 255.0f;
            const float b = static_cast<float>(raw[i * 4 + 2]) / 255.0f;
            const std::uint8_t a = raw[i * 4 + 3];

            sm::float3 linear{r, g, b};
            switch (colorSpace) {
            case TextureColorSpace::SrgbRec709Scene:
                linear = ColorSpace::srgbToLinearRec709(linear); // decode sRGB OETF
                if (toRec2020)
                    linear = ColorSpace::rec709ToRec2020(linear);
                break;
            case TextureColorSpace::LinRec709Scene:
                if (toRec2020)
                    linear = ColorSpace::rec709ToRec2020(linear);
                break;
            case TextureColorSpace::LinRec2020Scene:
                if (!toRec2020)
                    linear = ColorSpace::rec2020ToRec709(linear);
                break;
            default:
                break;
            }

            converted[i * 4 + 0] = static_cast<std::uint8_t>(std::lround(std::clamp(linear.r, 0.0f, 1.0f) * 255.0f));
            converted[i * 4 + 1] = static_cast<std::uint8_t>(std::lround(std::clamp(linear.g, 0.0f, 1.0f) * 255.0f));
            converted[i * 4 + 2] = static_cast<std::uint8_t>(std::lround(std::clamp(linear.b, 0.0f, 1.0f) * 255.0f));
            converted[i * 4 + 3] = a;
        }
    }

    const auto bytes = std::as_bytes(std::span<const std::uint8_t>(converted));
    return create(ctx,
                  cmdPool,
                  bytes,
                  static_cast<std::uint32_t>(w),
                  static_cast<std::uint32_t>(h),
                  name.empty() ? path.filename().string() : std::string(name));
}

void Texture::reset() noexcept {
    if (m_ctx != nullptr && m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_ctx->device, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
    m_ctx = nullptr;
    m_width = 0;
    m_height = 0;
    m_mipLevels = 1;
}
