#ifndef HARMONIA_SCENE_TEXTURE_HPP
#define HARMONIA_SCENE_TEXTURE_HPP

#include <volk/volk.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/CommandPool.hpp"
#include "harmonia/core/Image.hpp"
#include "harmonia/core/VulkanHandle.hpp"
#include "harmonia/utils/ColorSpace.hpp"

namespace harmonia {

/// Source color space of a texture asset.
/// Tokens are ASWF ColorInterop interop IDs (scene-referred set); only the
/// Rec.709/Rec.2020 related spaces are supported.
/// On load, all color data is converted to the scene's (linear) working color
/// space. Data maps (normal, ORM, roughness) use Data — no conversion applies.
enum class TextureColorSpace : std::uint8_t {
    Data = 0,        ///< "data"              — uninterpreted; no conversion
    SrgbRec709Scene, ///< "srgb_rec709_scene" — sRGB OETF, Rec.709 primaries
    LinRec709Scene,  ///< "lin_rec709_scene"  — linear, Rec.709 primaries
    LinRec2020Scene, ///< "lin_rec2020_scene" — linear, Rec.2020 primaries
};

/// Parse a ColorInterop interop ID.
/// Returns Data for unrecognised strings (safe fallback for data maps).
[[nodiscard]] inline TextureColorSpace parseTextureColorSpace(std::string_view name) noexcept {
    if (name == "srgb_rec709_scene")
        return TextureColorSpace::SrgbRec709Scene;
    if (name == "lin_rec709_scene")
        return TextureColorSpace::LinRec709Scene;
    if (name == "lin_rec2020_scene")
        return TextureColorSpace::LinRec2020Scene;
    return TextureColorSpace::Data;
}

class Texture {
  public:
    Texture() = default;
    ~Texture() noexcept = default;

    Texture(Texture&&) noexcept = default;
    Texture& operator=(Texture&&) noexcept = default;

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    /// Upload pre-decoded pixels (RGBA8, already in render color space).
    [[nodiscard]] static std::expected<Texture, VkResult> create(const DeviceContext& ctx,
                                                                 const CommandPool& cmdPool,
                                                                 std::span<const std::byte> pixels,
                                                                 std::uint32_t width,
                                                                 std::uint32_t height,
                                                                 std::string_view name = "");

    /// Load a texture from a file and convert it to the scene's (linear)
    /// working color space at load time. colorSpace describes how the source
    /// data is encoded; the correct decode + primaries conversion is applied
    /// on the CPU before GPU upload.
    [[nodiscard]] static std::expected<Texture, VkResult>
    loadFromFile(const DeviceContext& ctx,
                 const CommandPool& cmdPool,
                 const std::filesystem::path& path,
                 TextureColorSpace colorSpace = TextureColorSpace::SrgbRec709Scene,
                 harmonia::ColorSpace::WorkingColorSpace workingSpace = harmonia::ColorSpace::WorkingColorSpace::LinRec2020,
                 std::string_view name = "");

    [[nodiscard]] const Image& image() const noexcept { return m_image; }
    [[nodiscard]] VkSampler sampler() const noexcept { return m_sampler; }
    [[nodiscard]] std::uint32_t width() const noexcept { return m_width; }
    [[nodiscard]] std::uint32_t height() const noexcept { return m_height; }
    [[nodiscard]] std::uint32_t mipLevels() const noexcept { return m_mipLevels; }

  private:
    Image m_image{};
    harmonia::UniqueSampler m_sampler;
    std::uint32_t m_width{};
    std::uint32_t m_height{};
    std::uint32_t m_mipLevels{1};
};

} // namespace harmonia

#endif // HARMONIA_SCENE_TEXTURE_HPP
