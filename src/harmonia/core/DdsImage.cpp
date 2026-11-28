#include "harmonia/core/DdsImage.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>

#include "harmonia/core/HostImageUpload.hpp"

namespace harmonia::core {

namespace {

// File layout of the DirectDraw Surface format (DirectXTex DDS.h). Every integer is
// little-endian; the magic number is followed by the 124-byte DDS_HEADER.
constexpr std::uint32_t kMagic = 0x20534444U; // "DDS "
constexpr std::size_t kHeaderSize = 128U;     // magic + DDS_HEADER
constexpr std::size_t kDx10HeaderSize = 148U; // + DDS_HEADER_DXT10
constexpr std::uint32_t kDdsHeaderSize = 124U;
constexpr std::uint32_t kPixelFormatSize = 32U;

// Offsets from the start of the file.
constexpr std::size_t kOffsetHeaderSize = 4U;
constexpr std::size_t kOffsetHeight = 12U;
constexpr std::size_t kOffsetWidth = 16U;
constexpr std::size_t kOffsetMipMapCount = 28U;
constexpr std::size_t kOffsetPixelFormatSize = 76U;
constexpr std::size_t kOffsetPixelFormatFlags = 80U;
constexpr std::size_t kOffsetFourCc = 84U;
constexpr std::size_t kOffsetCaps2 = 112U;
constexpr std::size_t kOffsetDxgiFormat = 128U;
constexpr std::size_t kOffsetResourceDimension = 132U;
constexpr std::size_t kOffsetMiscFlag = 136U;
constexpr std::size_t kOffsetArraySize = 140U;

constexpr std::uint32_t kPfFourCc = 0x4U;         // DDPF_FOURCC
constexpr std::uint32_t kCaps2Cubemap = 0x200U;   // DDSCAPS2_CUBEMAP
constexpr std::uint32_t kCaps2Volume = 0x200000U; // DDSCAPS2_VOLUME
constexpr std::uint32_t kDimensionTexture2D = 3U; // D3D10_RESOURCE_DIMENSION_TEXTURE2D
constexpr std::uint32_t kMiscTextureCube = 0x4U;  // D3D11_RESOURCE_MISC_TEXTURECUBE

/// A block-compressed DXGI format and its Vulkan formats.
struct BlockFormat {
    VkFormat format;     ///< blocks read linearly
    VkFormat srgbFormat; ///< blocks decoded as sRGB by the hardware; VK_FORMAT_UNDEFINED when none exists
    bool fileIsSrgb;     ///< the file declares an sRGB encoding
};

/// DXGI_FORMAT values of the supported block-compressed families.
namespace dxgi {
constexpr std::uint32_t kBC1Typeless = 70U;
constexpr std::uint32_t kBC1Unorm = 71U;
constexpr std::uint32_t kBC1UnormSrgb = 72U;
constexpr std::uint32_t kBC2Typeless = 73U;
constexpr std::uint32_t kBC2Unorm = 74U;
constexpr std::uint32_t kBC2UnormSrgb = 75U;
constexpr std::uint32_t kBC3Typeless = 76U;
constexpr std::uint32_t kBC3Unorm = 77U;
constexpr std::uint32_t kBC3UnormSrgb = 78U;
constexpr std::uint32_t kBC4Typeless = 79U;
constexpr std::uint32_t kBC4Unorm = 80U;
constexpr std::uint32_t kBC4Snorm = 81U;
constexpr std::uint32_t kBC5Typeless = 82U;
constexpr std::uint32_t kBC5Unorm = 83U;
constexpr std::uint32_t kBC5Snorm = 84U;
constexpr std::uint32_t kBC6HUf16 = 95U;
constexpr std::uint32_t kBC6HSf16 = 96U;
constexpr std::uint32_t kBC7Typeless = 97U;
constexpr std::uint32_t kBC7Unorm = 98U;
constexpr std::uint32_t kBC7UnormSrgb = 99U;
} // namespace dxgi

/// The DXGI formats the parser accepts. A TYPELESS variant of an 8-bit family reads as its
/// UNORM sibling. DXGI_FORMAT_BC6H_TYPELESS is not listed: its blocks are either BC6H_UF16
/// or BC6H_SF16 and the file does not say which.
[[nodiscard]] std::optional<BlockFormat> dxgiBlockFormat(std::uint32_t dxgiFormat) noexcept {
    switch (dxgiFormat) {
    case dxgi::kBC1Typeless:
    case dxgi::kBC1Unorm:
        return BlockFormat{VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, false};
    case dxgi::kBC1UnormSrgb:
        return BlockFormat{VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, true};
    case dxgi::kBC2Typeless:
    case dxgi::kBC2Unorm:
        return BlockFormat{VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC2_SRGB_BLOCK, false};
    case dxgi::kBC2UnormSrgb:
        return BlockFormat{VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC2_SRGB_BLOCK, true};
    case dxgi::kBC3Typeless:
    case dxgi::kBC3Unorm:
        return BlockFormat{VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK, false};
    case dxgi::kBC3UnormSrgb:
        return BlockFormat{VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK, true};
    case dxgi::kBC4Typeless:
    case dxgi::kBC4Unorm:
        return BlockFormat{VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_UNDEFINED, false};
    case dxgi::kBC4Snorm:
        return BlockFormat{VK_FORMAT_BC4_SNORM_BLOCK, VK_FORMAT_UNDEFINED, false};
    case dxgi::kBC5Typeless:
    case dxgi::kBC5Unorm:
        return BlockFormat{VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_UNDEFINED, false};
    case dxgi::kBC5Snorm:
        return BlockFormat{VK_FORMAT_BC5_SNORM_BLOCK, VK_FORMAT_UNDEFINED, false};
    case dxgi::kBC6HUf16:
        return BlockFormat{VK_FORMAT_BC6H_UFLOAT_BLOCK, VK_FORMAT_UNDEFINED, false};
    case dxgi::kBC6HSf16:
        return BlockFormat{VK_FORMAT_BC6H_SFLOAT_BLOCK, VK_FORMAT_UNDEFINED, false};
    case dxgi::kBC7Typeless:
    case dxgi::kBC7Unorm:
        return BlockFormat{VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK, false};
    case dxgi::kBC7UnormSrgb:
        return BlockFormat{VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK, true};
    default:
        return std::nullopt;
    }
}

[[nodiscard]] constexpr std::uint32_t fourCc(char a, char b, char c, char d) noexcept {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(a)) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)) << 8U) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << 16U) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(d)) << 24U);
}

/// The FourCC codes of the legacy (pre-DX10) header that name a block-compressed format
/// without a channel swizzle, as DirectXTex's g_LegacyDDSMap maps them. The swizzled DXT5
/// variants ('A2D5', 'xGBR', ...) are not listed: their channels would be read wrong.
[[nodiscard]] std::optional<BlockFormat> legacyBlockFormat(std::uint32_t code) noexcept {
    if (code == fourCc('D', 'X', 'T', '1')) {
        return BlockFormat{VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, false};
    }
    if (code == fourCc('D', 'X', 'T', '2') || code == fourCc('D', 'X', 'T', '3')) {
        return BlockFormat{VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC2_SRGB_BLOCK, false};
    }
    if (code == fourCc('D', 'X', 'T', '4') || code == fourCc('D', 'X', 'T', '5')) {
        return BlockFormat{VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK, false};
    }
    if (code == fourCc('B', 'C', '4', 'U') || code == fourCc('A', 'T', 'I', '1')) {
        return BlockFormat{VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_UNDEFINED, false};
    }
    if (code == fourCc('B', 'C', '4', 'S')) {
        return BlockFormat{VK_FORMAT_BC4_SNORM_BLOCK, VK_FORMAT_UNDEFINED, false};
    }
    if (code == fourCc('B', 'C', '5', 'U') || code == fourCc('A', 'T', 'I', '2') ||
        code == fourCc('A', '2', 'X', 'Y')) {
        return BlockFormat{VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_UNDEFINED, false};
    }
    if (code == fourCc('B', 'C', '5', 'S')) {
        return BlockFormat{VK_FORMAT_BC5_SNORM_BLOCK, VK_FORMAT_UNDEFINED, false};
    }
    if (code == fourCc('B', 'C', '6', 'H')) {
        return BlockFormat{VK_FORMAT_BC6H_UFLOAT_BLOCK, VK_FORMAT_UNDEFINED, false};
    }
    if (code == fourCc('B', 'C', '7', 'L') || code == fourCc('B', 'C', '7', '\0')) {
        return BlockFormat{VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK, false};
    }
    return std::nullopt;
}

/// Little-endian 32-bit read of @p bytes at @p offset; the caller has checked the range.
[[nodiscard]] std::uint32_t read32(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    const auto byte = [&](std::size_t index) {
        return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + index]));
    };
    return byte(0U) | (byte(1U) << 8U) | (byte(2U) << 16U) | (byte(3U) << 24U);
}

} // namespace

std::string_view ddsErrorMessage(DdsError error) noexcept {
    switch (error) {
    case DdsError::NotADds:
        return "not a DDS file (missing the 'DDS ' magic)";
    case DdsError::TruncatedHeader:
        return "truncated header";
    case DdsError::InvalidHeader:
        return "inconsistent header (size, dimensions or mip count)";
    case DdsError::UnsupportedFormat:
        return "not a block-compressed BC1..BC7 format";
    case DdsError::UnsupportedLayout:
        return "not a single-layer 2D texture";
    case DdsError::TruncatedData:
        return "truncated: fewer bytes than the mip chain's blocks need";
    case DdsError::IoError:
        return "the file could not be read";
    }
    return "unknown error";
}

std::expected<DdsImage, DdsError> DdsImage::parse(std::vector<std::byte>&& bytes) {
    const std::span<const std::byte> file{bytes};
    if (file.size() < sizeof(kMagic) || read32(file, 0U) != kMagic) {
        return std::unexpected(DdsError::NotADds);
    }
    if (file.size() < kHeaderSize) {
        return std::unexpected(DdsError::TruncatedHeader);
    }
    if (read32(file, kOffsetHeaderSize) != kDdsHeaderSize || read32(file, kOffsetPixelFormatSize) != kPixelFormatSize) {
        return std::unexpected(DdsError::InvalidHeader);
    }

    const std::uint32_t width = read32(file, kOffsetWidth);
    const std::uint32_t height = read32(file, kOffsetHeight);
    const std::uint32_t declaredMips = read32(file, kOffsetMipMapCount);
    const std::uint32_t mipLevels = declaredMips == 0U ? 1U : declaredMips;
    if (width == 0U || height == 0U || mipLevels > fullMipChainLevels(VkExtent2D{width, height})) {
        return std::unexpected(DdsError::InvalidHeader);
    }

    std::size_t dataOffset = kHeaderSize;
    BlockFormat blockFormat{};
    const std::uint32_t pixelFormatFlags = read32(file, kOffsetPixelFormatFlags);
    const std::uint32_t code = read32(file, kOffsetFourCc);
    if ((pixelFormatFlags & kPfFourCc) != 0U && code == fourCc('D', 'X', '1', '0')) {
        if (file.size() < kDx10HeaderSize) {
            return std::unexpected(DdsError::TruncatedHeader);
        }
        if (read32(file, kOffsetResourceDimension) != kDimensionTexture2D ||
            (read32(file, kOffsetMiscFlag) & kMiscTextureCube) != 0U || read32(file, kOffsetArraySize) > 1U) {
            return std::unexpected(DdsError::UnsupportedLayout);
        }
        const auto format = dxgiBlockFormat(read32(file, kOffsetDxgiFormat));
        if (!format) {
            return std::unexpected(DdsError::UnsupportedFormat);
        }
        blockFormat = *format;
        dataOffset = kDx10HeaderSize;
    } else {
        if ((read32(file, kOffsetCaps2) & (kCaps2Cubemap | kCaps2Volume)) != 0U) {
            return std::unexpected(DdsError::UnsupportedLayout);
        }
        if ((pixelFormatFlags & kPfFourCc) == 0U) {
            return std::unexpected(DdsError::UnsupportedFormat);
        }
        const auto format = legacyBlockFormat(code);
        if (!format) {
            return std::unexpected(DdsError::UnsupportedFormat);
        }
        blockFormat = *format;
    }

    // Levels are stored one after another from the top level, every level as whole blocks.
    const TexelBlock block = texelBlock(blockFormat.format);
    std::vector<Level> levels;
    levels.reserve(mipLevels);
    std::uint64_t offset = dataOffset;
    std::uint32_t levelWidth = width;
    std::uint32_t levelHeight = height;
    for (std::uint32_t level = 0U; level < mipLevels; ++level) {
        const std::uint64_t byteLength = blockCount(block, levelWidth, levelHeight) * block.bytes;
        levels.push_back(Level{
            .byteOffset = offset,
            .byteLength = byteLength,
            .width = levelWidth,
            .height = levelHeight,
        });
        offset += byteLength;
        levelWidth = std::max(1U, levelWidth >> 1U);
        levelHeight = std::max(1U, levelHeight >> 1U);
    }
    if (file.size() < offset) {
        return std::unexpected(DdsError::TruncatedData);
    }

    DdsImage image;
    image.m_bytes = std::move(bytes);
    image.m_levels = std::move(levels);
    image.m_format = blockFormat.format;
    image.m_srgbFormat = blockFormat.srgbFormat;
    image.m_fileIsSrgb = blockFormat.fileIsSrgb;
    image.m_width = width;
    image.m_height = height;
    return image;
}

std::expected<DdsImage, DdsError> DdsImage::loadFromFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return std::unexpected(DdsError::IoError);
    }
    const std::streampos end = file.tellg();
    if (end == std::streampos(-1)) {
        return std::unexpected(DdsError::IoError);
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    file.seekg(0, std::ios::beg);
    if (!bytes.empty() &&
        !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        return std::unexpected(DdsError::IoError);
    }
    return parse(std::move(bytes));
}

VkFormat DdsImage::format(bool srgbDecode) const noexcept {
    return srgbDecode && m_srgbFormat != VK_FORMAT_UNDEFINED ? m_srgbFormat : m_format;
}

std::span<const std::byte> DdsImage::levelBytes(const Level& level) const noexcept {
    return std::span<const std::byte>{m_bytes.data() + static_cast<std::size_t>(level.byteOffset),
                                      static_cast<std::size_t>(level.byteLength)};
}

} // namespace harmonia::core
