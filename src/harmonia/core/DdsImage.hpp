#ifndef HARMONIA_CORE_DDSIMAGE_HPP
#define HARMONIA_CORE_DDSIMAGE_HPP

#include <volk/volk.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace harmonia::core {

/// Why a DDS file was not accepted by DdsImage.
enum class DdsError : std::uint8_t {
    NotADds,           ///< fewer than 4 bytes, or not the "DDS " magic
    TruncatedHeader,   ///< shorter than the 128-byte header, or than the DX10 extension it declares
    InvalidHeader,     ///< header sizes, dimensions or mip count are inconsistent
    UnsupportedFormat, ///< not a block-compressed BC1..BC7 format
    UnsupportedLayout, ///< not a single-layer 2D texture (array, cubemap, volume, 1D)
    TruncatedData,     ///< fewer bytes than the blocks of the mip chain need
    IoError,           ///< the file could not be read
};

/// Stable, human-readable reason of a DdsError.
[[nodiscard]] std::string_view ddsErrorMessage(DdsError error) noexcept;

/// A parsed DDS file: one block-compressed 2D image with its mip chain. The file bytes are
/// owned; a level is a byte range inside them. Blocks are never decoded.
class DdsImage {
  public:
    /// One mip level: a range of the file bytes holding its blocks, row-major from the top
    /// left, a partial 4x4 block at the right or bottom edge stored as a whole block.
    struct Level {
        std::uint64_t byteOffset{}; ///< from the start of bytes()
        std::uint64_t byteLength{}; ///< all blocks of the level
        std::uint32_t width{};      ///< level extent in texels
        std::uint32_t height{};
    };

    /// Parses @p bytes and keeps them; the levels point into the kept bytes.
    [[nodiscard]] static std::expected<DdsImage, DdsError> parse(std::vector<std::byte>&& bytes);
    /// Reads the whole file and parses it.
    [[nodiscard]] static std::expected<DdsImage, DdsError> loadFromFile(const std::filesystem::path& path);

    DdsImage() = default;
    DdsImage(const DdsImage&) = delete;
    DdsImage& operator=(const DdsImage&) = delete;
    DdsImage(DdsImage&&) noexcept = default;
    DdsImage& operator=(DdsImage&&) noexcept = default;

    /// Vulkan format of the blocks. With @p srgbDecode the hardware decodes sRGB
    /// (BC7_SRGB_BLOCK, ...); the file must then be one of the formats that has such a
    /// variant (hasSrgbVariant()). Without it the data is read linearly.
    [[nodiscard]] VkFormat format(bool srgbDecode) const noexcept;
    /// The file declares an sRGB encoding (a DXGI *_SRGB format).
    [[nodiscard]] bool fileIsSrgb() const noexcept { return m_fileIsSrgb; }
    /// The format family has a Vulkan variant with hardware sRGB decode (BC1, BC2, BC3, BC7).
    [[nodiscard]] bool hasSrgbVariant() const noexcept { return m_srgbFormat != VK_FORMAT_UNDEFINED; }

    [[nodiscard]] std::uint32_t width() const noexcept { return m_width; }
    [[nodiscard]] std::uint32_t height() const noexcept { return m_height; }
    [[nodiscard]] std::uint32_t mipLevels() const noexcept { return static_cast<std::uint32_t>(m_levels.size()); }
    /// Level 0 first; level i covers max(1, width >> i) x max(1, height >> i).
    [[nodiscard]] std::span<const Level> levels() const noexcept { return m_levels; }
    /// The blocks of @p level.
    [[nodiscard]] std::span<const std::byte> levelBytes(const Level& level) const noexcept;
    /// The whole file.
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return m_bytes; }

  private:
    std::vector<std::byte> m_bytes;
    std::vector<Level> m_levels;
    VkFormat m_format = VK_FORMAT_UNDEFINED;     ///< without hardware sRGB decode
    VkFormat m_srgbFormat = VK_FORMAT_UNDEFINED; ///< with hardware sRGB decode, when the family has one
    bool m_fileIsSrgb = false;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
};

} // namespace harmonia::core

#endif // HARMONIA_CORE_DDSIMAGE_HPP
