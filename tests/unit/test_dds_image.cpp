// Unit tests: harmonia::core::DdsImage — the DDS block-compressed parser.
//
// Pure CPU: no Vulkan device. Synthetic files are built byte by byte, so every rejection
// is provoked at its exact cause. The level layout oracle is a table of literal numbers
// written from the DDS definition (level i covers ceil(w / 2^i) x ceil(h / 2^i) texels,
// whole 4x4 blocks), independent of the production helpers. The DirectXTex fixtures of
// tests/assets/dds pin the parser to what texconv writes.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <optional>
#include <span>
#include <vector>

#include "harmonia/core/DdsImage.hpp"
#include "harmonia/core/HostImageUpload.hpp"

namespace {

using harmonia::core::DdsError;
using harmonia::core::DdsImage;

constexpr std::uint32_t kMagic = 0x20534444U; // "DDS "
constexpr std::uint32_t kDx10 = 0x30315844U;  // "DX10"
constexpr std::uint32_t kDimension2D = 3U;
constexpr std::uint32_t kDimension1D = 1U;
constexpr std::uint32_t kDimension3D = 4U;
constexpr std::uint32_t kMiscTextureCube = 0x4U;

// DXGI_FORMAT
constexpr std::uint32_t kBC7Unorm = 98U;
constexpr std::uint32_t kBC6HTypeless = 94U;
constexpr std::uint32_t kR8G8B8A8Unorm = 28U;

void put32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0U; index < 4U; ++index) {
        bytes[offset + index] = static_cast<std::byte>((value >> (8U * index)) & 0xFFU);
    }
}

/// A DX10 DDS with a well-formed header and @p dataBytes bytes of block data.
std::vector<std::byte> dx10Dds(std::uint32_t dxgiFormat,
                               std::uint32_t width,
                               std::uint32_t height,
                               std::uint32_t mips,
                               std::size_t dataBytes) {
    std::vector<std::byte> bytes(148U + dataBytes, std::byte{0xA5});
    put32(bytes, 0U, kMagic);
    put32(bytes, 4U, 124U);                                        // DDS_HEADER size
    put32(bytes, 8U, 0x2U | 0x4U | 0x1000U | 0x20000U | 0x80000U); // HEIGHT|WIDTH|PIXELFORMAT|MIPMAPCOUNT|LINEARSIZE
    put32(bytes, 12U, height);
    put32(bytes, 16U, width);
    put32(bytes, 28U, mips);
    put32(bytes, 76U, 32U);  // DDS_PIXELFORMAT size
    put32(bytes, 80U, 0x4U); // DDPF_FOURCC
    put32(bytes, 84U, kDx10);
    put32(bytes, 108U, 0x1000U); // caps: TEXTURE
    put32(bytes, 128U, dxgiFormat);
    put32(bytes, 132U, kDimension2D);
    put32(bytes, 136U, 0U); // miscFlag: no cubemap
    put32(bytes, 140U, 1U); // arraySize
    return bytes;
}

/// A legacy (pre-DX10) DDS whose pixel format is only a FourCC; data starts at byte 128.
std::vector<std::byte>
legacyDds(std::uint32_t fourCc, std::uint32_t width, std::uint32_t height, std::size_t dataBytes) {
    std::vector<std::byte> bytes(128U + dataBytes, std::byte{0x5A});
    put32(bytes, 0U, kMagic);
    put32(bytes, 4U, 124U);
    put32(bytes, 8U, 0x2U | 0x4U | 0x1000U | 0x80000U);
    put32(bytes, 12U, height);
    put32(bytes, 16U, width);
    put32(bytes, 28U, 1U); // one mip level
    put32(bytes, 76U, 32U);
    put32(bytes, 80U, 0x4U);
    put32(bytes, 84U, fourCc);
    put32(bytes, 108U, 0x1000U);
    put32(bytes, 112U, 0U); // caps2: no cubemap, no volume
    return bytes;
}

constexpr std::uint32_t fourCc(char a, char b, char c, char d) {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(a)) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)) << 8U) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << 16U) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(d)) << 24U);
}

struct ExpectedLevel {
    std::uint64_t byteOffset;
    std::uint64_t byteLength;
    std::uint32_t width;
    std::uint32_t height;
};

void expectLevels(const DdsImage& image, std::span<const ExpectedLevel> expected) {
    ASSERT_EQ(image.levels().size(), expected.size());
    for (std::size_t level = 0U; level < expected.size(); ++level) {
        SCOPED_TRACE(::testing::Message() << "level " << level);
        EXPECT_EQ(image.levels()[level].byteOffset, expected[level].byteOffset);
        EXPECT_EQ(image.levels()[level].byteLength, expected[level].byteLength);
        EXPECT_EQ(image.levels()[level].width, expected[level].width);
        EXPECT_EQ(image.levels()[level].height, expected[level].height);
    }
}

/// Block size in bytes of a BC format, from the format's definition: one 64-bit word for
/// BC1 and BC4, two for the others. Deliberately not core::texelBlock.
constexpr std::uint32_t texelBlockBytes(VkFormat format) noexcept {
    switch (format) {
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC4_SNORM_BLOCK:
        return 8U;
    default:
        return 16U;
    }
}

/// True when @p result is the rejection @p error; a different error, or an accepted file,
/// fails the test. (.error() on an accepted result would be undefined behaviour.)
bool expectRejection(const std::expected<DdsImage, DdsError>& result, DdsError error) {
    if (result.has_value()) {
        ADD_FAILURE() << "the file was accepted, expected: " << harmonia::core::ddsErrorMessage(error);
        return false;
    }
    if (result.error() != error) {
        ADD_FAILURE() << "rejected with '" << harmonia::core::ddsErrorMessage(result.error()) << "' instead of '"
                      << harmonia::core::ddsErrorMessage(error) << "'";
        return false;
    }
    return true;
}

std::filesystem::path fixture(const char* file) {
    return std::filesystem::path(HARMONIA_TEST_ASSETS_DIR) / "dds" / file;
}

} // namespace

// ── Block layout ───────────────────────────────────────────────────────────────

// 16x16 BC7, five levels: the literal offsets are the DDS definition applied by hand.
TEST(DdsImage, Dx10Bc7ChainHasTheDocumentedOffsets) {
    // 16x16 = 4x4 blocks (256 B), 8x8 = 2x2 (64 B), 4x4, 2x2 and 1x1 are one block (16 B each).
    constexpr ExpectedLevel kExpected[]{
        {148U, 256U, 16U, 16U},
        {404U, 64U, 8U, 8U},
        {468U, 16U, 4U, 4U},
        {484U, 16U, 2U, 2U},
        {500U, 16U, 1U, 1U},
    };
    auto image = DdsImage::parse(dx10Dds(kBC7Unorm, 16U, 16U, 5U, 368U));
    ASSERT_TRUE(image.has_value()) << harmonia::core::ddsErrorMessage(image.error());
    EXPECT_EQ(image->format(false), VK_FORMAT_BC7_UNORM_BLOCK);
    EXPECT_EQ(image->format(true), VK_FORMAT_BC7_SRGB_BLOCK);
    EXPECT_TRUE(image->hasSrgbVariant());
    EXPECT_FALSE(image->fileIsSrgb());
    EXPECT_EQ(image->width(), 16U);
    EXPECT_EQ(image->height(), 16U);
    EXPECT_EQ(image->mipLevels(), 5U);
    expectLevels(*image, kExpected);
}

// Dimensions that are not multiples of the block: every level rounds up to whole blocks.
TEST(DdsImage, PartialBlocksAreRoundedUpPerLevel) {
    // 18x10 = 5x3 blocks (240 B), 9x5 = 3x2 (96 B), 4x2 = 1x1 (16 B), 2x1 and 1x1 (16 B each).
    constexpr ExpectedLevel kExpected[]{
        {148U, 240U, 18U, 10U},
        {388U, 96U, 9U, 5U},
        {484U, 16U, 4U, 2U},
        {500U, 16U, 2U, 1U},
        {516U, 16U, 1U, 1U},
    };
    auto image = DdsImage::parse(dx10Dds(kBC7Unorm, 18U, 10U, 5U, 384U));
    ASSERT_TRUE(image.has_value()) << harmonia::core::ddsErrorMessage(image.error());
    expectLevels(*image, kExpected);
}

// The number of levels follows the longest side, not the shortest.
// Level sizes confirmed with texconv: 1x16 BC7 is 144 data bytes.
TEST(DdsImage, ChainLengthFollowsTheLongestSide) {
    // 1x16 is one block wide and four blocks tall (64 B), then 1x8 (32 B), then one block each.
    constexpr ExpectedLevel kExpected[]{
        {148U, 64U, 1U, 16U},
        {212U, 32U, 1U, 8U},
        {244U, 16U, 1U, 4U},
        {260U, 16U, 1U, 2U},
        {276U, 16U, 1U, 1U},
    };
    auto image = DdsImage::parse(dx10Dds(kBC7Unorm, 1U, 16U, 5U, 144U));
    ASSERT_TRUE(image.has_value()) << harmonia::core::ddsErrorMessage(image.error());
    expectLevels(*image, kExpected);
}

// Odd dimensions that are not multiples of the block: texconv writes 320 data bytes for a
// 17x9 BC7 chain, in the block counts below.
TEST(DdsImage, OddDimensionsRoundEveryLevelUpToWholeBlocks) {
    constexpr ExpectedLevel kExpected[]{
        {148U, 240U, 17U, 9U},
        {388U, 32U, 8U, 4U},
        {420U, 16U, 4U, 2U},
        {436U, 16U, 2U, 1U},
        {452U, 16U, 1U, 1U},
    };
    auto image = DdsImage::parse(dx10Dds(kBC7Unorm, 17U, 9U, 5U, 320U));
    ASSERT_TRUE(image.has_value()) << harmonia::core::ddsErrorMessage(image.error());
    expectLevels(*image, kExpected);
}

TEST(DdsImage, MipCountZeroMeansASingleLevel) {
    auto image = DdsImage::parse(dx10Dds(kBC7Unorm, 16U, 16U, 0U, 256U));
    ASSERT_TRUE(image.has_value()) << harmonia::core::ddsErrorMessage(image.error());
    EXPECT_EQ(image->mipLevels(), 1U);
    EXPECT_EQ(image->levels()[0].byteLength, 256U);
}

// A chain need not be complete: a prefix of the levels is valid and stops where it stops.
TEST(DdsImage, AcceptsAChainShorterThanTheCompleteOne) {
    auto image = DdsImage::parse(dx10Dds(kBC7Unorm, 16U, 16U, 2U, 320U));
    ASSERT_TRUE(image.has_value()) << harmonia::core::ddsErrorMessage(image.error());
    EXPECT_EQ(image->mipLevels(), 2U);
    EXPECT_EQ(image->levels()[1].byteOffset, 404U);
    EXPECT_EQ(image->levels()[1].byteLength, 64U);
}

// The shape of the official OpenPBR scene textures: texconv (CPU BC7) on a 4096x4096 PNG
// writes 13 levels, a DX10 header, data at 148, pitchOrLinearSize 16,777,216 and a file
// of 22,369,796 bytes. The literals below are the level table of that
// file, summed by hand: (1024^2 + 512^2 + ... + 2^2 + 1^2 + 1^2) blocks * 16 B + 148.
TEST(DdsImage, ParsesA4kChainWith13LevelsLikeTheSceneTextures) {
    constexpr std::uint64_t kChainBytes = 22369648ULL;
    auto bytes = dx10Dds(kBC7Unorm, 4096U, 4096U, 13U, static_cast<std::size_t>(kChainBytes));
    put32(bytes, 20U, 16777216U); // pitchOrLinearSize of the top level
    auto image = DdsImage::parse(std::move(bytes));
    ASSERT_TRUE(image.has_value()) << harmonia::core::ddsErrorMessage(image.error());
    EXPECT_EQ(image->format(false), VK_FORMAT_BC7_UNORM_BLOCK);
    EXPECT_EQ(image->mipLevels(), 13U);
    constexpr ExpectedLevel kExpected[]{
        {148ULL, 16777216ULL, 4096U, 4096U},
        {16777364ULL, 4194304ULL, 2048U, 2048U},
        {20971668ULL, 1048576ULL, 1024U, 1024U},
        {22020244ULL, 262144ULL, 512U, 512U},
        {22282388ULL, 65536ULL, 256U, 256U},
        {22347924ULL, 16384ULL, 128U, 128U},
        {22364308ULL, 4096ULL, 64U, 64U},
        {22368404ULL, 1024ULL, 32U, 32U},
        {22369428ULL, 256ULL, 16U, 16U},
        {22369684ULL, 64ULL, 8U, 8U},
        {22369748ULL, 16ULL, 4U, 4U},
        {22369764ULL, 16ULL, 2U, 2U},
        {22369780ULL, 16ULL, 1U, 1U},
    };
    expectLevels(*image, kExpected);
    // The last level ends exactly at the end of the file: 22,369,780 + 16.
    EXPECT_EQ(image->levels().back().byteOffset + image->levels().back().byteLength, 22369796ULL);
    EXPECT_EQ(image->bytes().size(), 22369796ULL);
}

TEST(DdsImage, LevelBytesAreTheFileSliceOfTheLevel) {
    auto bytes = dx10Dds(kBC7Unorm, 16U, 16U, 5U, 368U);
    // Distinct content per level so that a wrong slice is visible (level 0 ends at 404).
    std::fill(bytes.begin() + 368, bytes.begin() + 404, std::byte{0x11}); // level 0 ends at 404
    std::fill(bytes.begin() + 404, bytes.begin() + 468, std::byte{0x22});
    std::fill(bytes.begin() + 468, bytes.end(), std::byte{0x33});
    auto image = DdsImage::parse(std::move(bytes));
    ASSERT_TRUE(image.has_value());
    const auto level1 = image->levelBytes(image->levels()[1]);
    ASSERT_EQ(level1.size(), 64U);
    EXPECT_TRUE(std::all_of(level1.begin(), level1.end(), [](std::byte value) { return value == std::byte{0x22}; }));
    EXPECT_EQ(image->bytes().size(), 148U + 368U);
}

// ── Format mapping ─────────────────────────────────────────────────────────────

struct FormatCase {
    std::uint32_t dxgiFormat;
    VkFormat linear;
    VkFormat srgb;
    bool fileIsSrgb;
};

class DdsImageDxgiFormat : public ::testing::TestWithParam<FormatCase> {};

TEST_P(DdsImageDxgiFormat, MapsToTheVulkanFamily) {
    const auto& format = GetParam();
    const std::size_t dataBytes = 16U * texelBlockBytes(format.linear);
    auto image = DdsImage::parse(dx10Dds(format.dxgiFormat, 16U, 16U, 1U, dataBytes));
    ASSERT_TRUE(image.has_value()) << harmonia::core::ddsErrorMessage(image.error());
    EXPECT_EQ(image->format(false), format.linear);
    EXPECT_EQ(image->format(true), format.srgb != VK_FORMAT_UNDEFINED ? format.srgb : format.linear);
    EXPECT_EQ(image->hasSrgbVariant(), format.srgb != VK_FORMAT_UNDEFINED);
    EXPECT_EQ(image->fileIsSrgb(), format.fileIsSrgb);
}

INSTANTIATE_TEST_SUITE_P(
    Dxgi,
    DdsImageDxgiFormat,
    ::testing::Values(
        FormatCase{70U, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, false}, // BC1_TYPELESS
        FormatCase{71U, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, false},
        FormatCase{72U, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, true},
        FormatCase{74U, VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC2_SRGB_BLOCK, false},
        FormatCase{75U, VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC2_SRGB_BLOCK, true},
        FormatCase{77U, VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK, false},
        FormatCase{78U, VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK, true},
        FormatCase{80U, VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_UNDEFINED, false},
        FormatCase{81U, VK_FORMAT_BC4_SNORM_BLOCK, VK_FORMAT_UNDEFINED, false},
        FormatCase{83U, VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_UNDEFINED, false},
        FormatCase{84U, VK_FORMAT_BC5_SNORM_BLOCK, VK_FORMAT_UNDEFINED, false},
        FormatCase{95U, VK_FORMAT_BC6H_UFLOAT_BLOCK, VK_FORMAT_UNDEFINED, false},
        FormatCase{96U, VK_FORMAT_BC6H_SFLOAT_BLOCK, VK_FORMAT_UNDEFINED, false},
        FormatCase{98U, VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK, false},
        FormatCase{99U, VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK, true}));

TEST(DdsImage, LegacyFourCcNamesTheBlockFamily) {
    // DXT1 (BC1) blocks are 8 bytes: 16x16 = 4x4 blocks = 128 B, data at byte 128.
    auto dxt1 = DdsImage::parse(legacyDds(fourCc('D', 'X', 'T', '1'), 16U, 16U, 128U));
    ASSERT_TRUE(dxt1.has_value()) << harmonia::core::ddsErrorMessage(dxt1.error());
    EXPECT_EQ(dxt1->format(false), VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
    EXPECT_EQ(dxt1->levels()[0].byteOffset, 128U);
    EXPECT_EQ(dxt1->levels()[0].byteLength, 128U);

    auto dxt5 = DdsImage::parse(legacyDds(fourCc('D', 'X', 'T', '5'), 16U, 16U, 256U));
    ASSERT_TRUE(dxt5.has_value());
    EXPECT_EQ(dxt5->format(false), VK_FORMAT_BC3_UNORM_BLOCK);

    auto bc4 = DdsImage::parse(legacyDds(fourCc('B', 'C', '4', 'U'), 16U, 16U, 128U));
    ASSERT_TRUE(bc4.has_value());
    EXPECT_EQ(bc4->format(false), VK_FORMAT_BC4_UNORM_BLOCK);

    auto bc7 = DdsImage::parse(legacyDds(fourCc('B', 'C', '7', 'L'), 16U, 16U, 256U));
    ASSERT_TRUE(bc7.has_value());
    EXPECT_EQ(bc7->format(false), VK_FORMAT_BC7_UNORM_BLOCK);
}

// ── Rejections ─────────────────────────────────────────────────────────────────

TEST(DdsImage, RejectsEverythingThatIsNotADds) {
    EXPECT_TRUE(expectRejection(DdsImage::parse(std::vector<std::byte>{}), DdsError::NotADds));
    EXPECT_TRUE(expectRejection(DdsImage::parse(std::vector<std::byte>(3U, std::byte{'D'})), DdsError::NotADds));
    std::vector<std::byte> png{std::byte{0x89}, std::byte{'P'}, std::byte{'N'}, std::byte{'G'}, std::byte{0}};
    EXPECT_TRUE(expectRejection(DdsImage::parse(std::move(png)), DdsError::NotADds));
}

TEST(DdsImage, RejectsAFileShorterThanItsHeader) {
    std::vector<std::byte> truncated(127U, std::byte{0});
    put32(truncated, 0U, kMagic);
    EXPECT_TRUE(expectRejection(DdsImage::parse(std::move(truncated)), DdsError::TruncatedHeader));
}

TEST(DdsImage, RejectsDx10WithoutTheExtensionHeader) {
    // Otherwise a valid 16x16 single-level header: the only defect is the missing extension.
    std::vector<std::byte> bytes(147U, std::byte{0});
    put32(bytes, 0U, kMagic);
    put32(bytes, 4U, 124U);
    put32(bytes, 12U, 16U);
    put32(bytes, 16U, 16U);
    put32(bytes, 28U, 1U);
    put32(bytes, 76U, 32U);
    put32(bytes, 80U, 0x4U);
    put32(bytes, 84U, kDx10);
    EXPECT_TRUE(expectRejection(DdsImage::parse(std::move(bytes)), DdsError::TruncatedHeader));
}

TEST(DdsImage, RejectsWrongHeaderSizes) {
    auto wrongHeaderSize = dx10Dds(kBC7Unorm, 16U, 16U, 1U, 256U);
    put32(wrongHeaderSize, 4U, 123U);
    EXPECT_TRUE(expectRejection(DdsImage::parse(std::move(wrongHeaderSize)), DdsError::InvalidHeader));

    auto wrongPixelFormatSize = dx10Dds(kBC7Unorm, 16U, 16U, 1U, 256U);
    put32(wrongPixelFormatSize, 76U, 31U);
    EXPECT_TRUE(expectRejection(DdsImage::parse(std::move(wrongPixelFormatSize)), DdsError::InvalidHeader));
}

TEST(DdsImage, RejectsZeroDimensions) {
    EXPECT_TRUE(expectRejection(DdsImage::parse(dx10Dds(kBC7Unorm, 0U, 16U, 1U, 16U)), DdsError::InvalidHeader));
    EXPECT_TRUE(expectRejection(DdsImage::parse(dx10Dds(kBC7Unorm, 16U, 0U, 1U, 16U)), DdsError::InvalidHeader));
}

// A chain cannot have more levels than the dimensions allow.
TEST(DdsImage, RejectsAMipCountBeyondTheChainOfTheDimensions) {
    // 17x9 has five levels (17, 8, 4, 2, 1); the sixth does not exist.
    EXPECT_TRUE(expectRejection(DdsImage::parse(dx10Dds(kBC7Unorm, 17U, 9U, 6U, 1024U)), DdsError::InvalidHeader));
    EXPECT_TRUE(DdsImage::parse(dx10Dds(kBC7Unorm, 17U, 9U, 5U, 1024U)).has_value());
}

TEST(DdsImage, RejectsUncompressedAmbiguousAndUnknownFormats) {
    // R8G8B8A8_UNORM: a fine texture, but not block compressed.
    EXPECT_TRUE(
        expectRejection(DdsImage::parse(dx10Dds(kR8G8B8A8Unorm, 16U, 16U, 1U, 1024U)), DdsError::UnsupportedFormat));
    // BC6H_TYPELESS does not say whether the blocks are UF16 or SF16.
    EXPECT_TRUE(
        expectRejection(DdsImage::parse(dx10Dds(kBC6HTypeless, 16U, 16U, 1U, 256U)), DdsError::UnsupportedFormat));
    // DXGI_FORMAT_UNKNOWN and a value outside the enum.
    EXPECT_TRUE(expectRejection(DdsImage::parse(dx10Dds(0U, 16U, 16U, 1U, 256U)), DdsError::UnsupportedFormat));
    EXPECT_TRUE(expectRejection(DdsImage::parse(dx10Dds(4000U, 16U, 16U, 1U, 256U)), DdsError::UnsupportedFormat));
}

TEST(DdsImage, RejectsNonSingle2DLayouts) {
    auto array = dx10Dds(kBC7Unorm, 16U, 16U, 1U, 512U);
    put32(array, 140U, 2U); // arraySize
    EXPECT_TRUE(expectRejection(DdsImage::parse(std::move(array)), DdsError::UnsupportedLayout));

    auto volume = dx10Dds(kBC7Unorm, 16U, 16U, 1U, 256U);
    put32(volume, 132U, kDimension3D);
    EXPECT_TRUE(expectRejection(DdsImage::parse(std::move(volume)), DdsError::UnsupportedLayout));

    auto oneDimensional = dx10Dds(kBC7Unorm, 16U, 16U, 1U, 256U);
    put32(oneDimensional, 132U, kDimension1D);
    EXPECT_TRUE(expectRejection(DdsImage::parse(std::move(oneDimensional)), DdsError::UnsupportedLayout));

    auto cubemap = dx10Dds(kBC7Unorm, 16U, 16U, 1U, 256U);
    put32(cubemap, 136U, kMiscTextureCube);
    EXPECT_TRUE(expectRejection(DdsImage::parse(std::move(cubemap)), DdsError::UnsupportedLayout));

    // A legacy cubemap or volume is declared in caps2 (offset 112).
    auto legacyCube = legacyDds(fourCc('D', 'X', 'T', '1'), 16U, 16U, 128U);
    put32(legacyCube, 112U, 0x200U); // DDSCAPS2_CUBEMAP
    EXPECT_TRUE(expectRejection(DdsImage::parse(std::move(legacyCube)), DdsError::UnsupportedLayout));
}

TEST(DdsImage, RejectsLegacyUncompressedPixelFormats) {
    // DDPF_RGB without a FourCC is an uncompressed legacy format.
    auto rgb = legacyDds(0U, 16U, 16U, 1024U);
    put32(rgb, 80U, 0x40U);
    EXPECT_TRUE(expectRejection(DdsImage::parse(std::move(rgb)), DdsError::UnsupportedFormat));
    // A FourCC outside the block-compressed set.
    EXPECT_TRUE(expectRejection(DdsImage::parse(legacyDds(fourCc('A', '2', 'D', '5'), 16U, 16U, 256U)),
                                DdsError::UnsupportedFormat));
}

TEST(DdsImage, RejectsAFileShorterThanItsBlocks) {
    // One byte, one block and one whole level missing.
    EXPECT_TRUE(expectRejection(DdsImage::parse(dx10Dds(kBC7Unorm, 16U, 16U, 5U, 367U)), DdsError::TruncatedData));
    EXPECT_TRUE(expectRejection(DdsImage::parse(dx10Dds(kBC7Unorm, 16U, 16U, 5U, 352U)), DdsError::TruncatedData));
    EXPECT_TRUE(expectRejection(DdsImage::parse(dx10Dds(kBC7Unorm, 16U, 16U, 5U, 320U)), DdsError::TruncatedData));
    // The exact size passes; trailing padding is tolerated, as by DirectXTex.
    EXPECT_TRUE(DdsImage::parse(dx10Dds(kBC7Unorm, 16U, 16U, 5U, 368U)).has_value());
    EXPECT_TRUE(DdsImage::parse(dx10Dds(kBC7Unorm, 16U, 16U, 5U, 372U)).has_value());
}

TEST(DdsImage, ReportsAnUnreadableFile) {
    const auto missing = std::filesystem::temp_directory_path() / "harmonia_dds_unit_tests" / "does_not_exist.dds";
    std::filesystem::remove(missing);
    EXPECT_TRUE(expectRejection(DdsImage::loadFromFile(missing), DdsError::IoError));
}

// ── DirectXTex fixtures ────────────────────────────────────────────────────────

TEST(DdsImage, ParsesTheTexconvBc7Fixture) {
    auto image = DdsImage::loadFromFile(fixture("bc7_checker16.dds"));
    ASSERT_TRUE(image.has_value()) << harmonia::core::ddsErrorMessage(image.error());
    EXPECT_EQ(image->format(false), VK_FORMAT_BC7_UNORM_BLOCK);
    EXPECT_FALSE(image->fileIsSrgb());
    EXPECT_EQ(image->width(), 16U);
    EXPECT_EQ(image->height(), 16U);
    constexpr ExpectedLevel kExpected[]{
        {148U, 256U, 16U, 16U},
        {404U, 64U, 8U, 8U},
        {468U, 16U, 4U, 4U},
        {484U, 16U, 2U, 2U},
        {500U, 16U, 1U, 1U},
    };
    expectLevels(*image, kExpected);
    EXPECT_EQ(image->bytes().size(), 516U);
}

TEST(DdsImage, ParsesTheTexconvBc7SrgbFixture) {
    auto image = DdsImage::loadFromFile(fixture("bc7_checker16_srgb.dds"));
    ASSERT_TRUE(image.has_value()) << harmonia::core::ddsErrorMessage(image.error());
    EXPECT_TRUE(image->fileIsSrgb());
    EXPECT_EQ(image->format(false), VK_FORMAT_BC7_UNORM_BLOCK);
    EXPECT_EQ(image->format(true), VK_FORMAT_BC7_SRGB_BLOCK);
}

TEST(DdsImage, ParsesTheTexconvBc1AndBc5Fixtures) {
    auto bc1 = DdsImage::loadFromFile(fixture("bc7_checker16_bc1.dds"));
    ASSERT_TRUE(bc1.has_value()) << harmonia::core::ddsErrorMessage(bc1.error());
    EXPECT_EQ(bc1->format(false), VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
    EXPECT_EQ(bc1->levels()[0].byteOffset, 128U); // legacy header
    EXPECT_EQ(bc1->levels()[0].byteLength, 128U);

    auto bc5 = DdsImage::loadFromFile(fixture("bc7_checker16_bc5.dds"));
    ASSERT_TRUE(bc5.has_value()) << harmonia::core::ddsErrorMessage(bc5.error());
    EXPECT_EQ(bc5->format(false), VK_FORMAT_BC5_UNORM_BLOCK);
    EXPECT_FALSE(bc5->hasSrgbVariant()); // no sRGB decode exists for BC5
}

// Negative control: the uncompressed reference of the GPU test is a real DDS that the
// compressed path must refuse rather than decode.
TEST(DdsImage, RejectsTheUncompressedReferenceFixture) {
    EXPECT_TRUE(expectRejection(DdsImage::loadFromFile(fixture("bc7_checker16_ref.dds")), DdsError::UnsupportedFormat));
}
