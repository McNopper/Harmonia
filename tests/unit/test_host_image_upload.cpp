// Unit tests: the pure helpers of harmonia::core::HostImageUpload that size a
// caller-supplied mip chain and decide whether a format can host one.
//
// Pure CPU: no Vulkan device. The format-support predicate is driven with synthetic
// VkFormatFeatureFlags, which is the only way to exercise an absent feature on a GPU that
// supports everything the loader needs.

#include <volk/volk.h>

#include <cstdint>
#include <gtest/gtest.h>

#include "harmonia/core/HostImageUpload.hpp"

namespace {

using harmonia::core::fullMipChainLevels;
using harmonia::core::mipChainFormatSupported;
using harmonia::core::TexelBlock;
using harmonia::core::texelBlock;

constexpr TexelBlock kNone{};
constexpr TexelBlock kBc1{4U, 4U, 8U};
constexpr TexelBlock kBc7{4U, 4U, 16U};
constexpr TexelBlock kRgba8{1U, 1U, 4U};
constexpr TexelBlock kRgba16{1U, 1U, 8U};
constexpr TexelBlock kRgba32f{1U, 1U, 16U};

} // namespace

// ── texelBlock ─────────────────────────────────────────────────────────────────

TEST(TexelBlock, BlockCompressedFormatsHaveA4x4Extent) {
    for (const VkFormat format : {VK_FORMAT_BC1_RGB_UNORM_BLOCK,
                                  VK_FORMAT_BC1_RGB_SRGB_BLOCK,
                                  VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
                                  VK_FORMAT_BC1_RGBA_SRGB_BLOCK,
                                  VK_FORMAT_BC4_UNORM_BLOCK,
                                  VK_FORMAT_BC4_SNORM_BLOCK}) {
        EXPECT_EQ(texelBlock(format), kBc1) << "format " << static_cast<int>(format);
    }
    for (const VkFormat format : {VK_FORMAT_BC2_UNORM_BLOCK,
                                  VK_FORMAT_BC2_SRGB_BLOCK,
                                  VK_FORMAT_BC3_UNORM_BLOCK,
                                  VK_FORMAT_BC3_SRGB_BLOCK,
                                  VK_FORMAT_BC5_UNORM_BLOCK,
                                  VK_FORMAT_BC5_SNORM_BLOCK,
                                  VK_FORMAT_BC6H_UFLOAT_BLOCK,
                                  VK_FORMAT_BC6H_SFLOAT_BLOCK,
                                  VK_FORMAT_BC7_UNORM_BLOCK,
                                  VK_FORMAT_BC7_SRGB_BLOCK}) {
        EXPECT_EQ(texelBlock(format), kBc7) << "format " << static_cast<int>(format);
    }
}

TEST(TexelBlock, TheUploadedUncompressedFormatsAreOneTexelPerBlock) {
    EXPECT_EQ(texelBlock(VK_FORMAT_R8G8B8A8_UNORM), kRgba8);
    EXPECT_EQ(texelBlock(VK_FORMAT_R8G8B8A8_SRGB), kRgba8);
    EXPECT_EQ(texelBlock(VK_FORMAT_R16G16B16A16_UNORM), kRgba16);
    EXPECT_EQ(texelBlock(VK_FORMAT_R16G16B16A16_SFLOAT), kRgba16);
    EXPECT_EQ(texelBlock(VK_FORMAT_R32G32B32A32_SFLOAT), kRgba32f);
}

// A format of unknown footprint must be reported as {0, 0, 0}, never guessed.
TEST(TexelBlock, FormatsOutsideTheTableHaveNoFootprint) {
    EXPECT_EQ(texelBlock(VK_FORMAT_UNDEFINED), kNone);
    EXPECT_EQ(texelBlock(VK_FORMAT_R8_UNORM), kNone);
    EXPECT_EQ(texelBlock(VK_FORMAT_ASTC_4x4_UNORM_BLOCK), kNone);
}

// ── blockCount ─────────────────────────────────────────────────────────────────

TEST(BlockCount, RoundsPartialBlocksUp) {
    using harmonia::core::blockCount;
    EXPECT_EQ(blockCount(kBc7, 16U, 16U), 16U);
    EXPECT_EQ(blockCount(kBc7, 1U, 1U), 1U);
    EXPECT_EQ(blockCount(kBc7, 4096U, 4096U), 1048576U);
    // 18 texels need five blocks, 10 need three.
    EXPECT_EQ(blockCount(kBc7, 18U, 10U), 15U);
    // The 1x1 tail levels of a chain of a 4096 texture are one block each.
    EXPECT_EQ(blockCount(kBc7, 2U, 2U), 1U);
    EXPECT_EQ(blockCount(kBc1, 20U, 4U), 5U);
    EXPECT_EQ(blockCount(kRgba8, 3U, 2U), 6U);
}

TEST(BlockCount, DoesNotOverflowOnLargeDimensions) {
    // 2^32 - 1 texels per side: (2^30) * (2^30) blocks = 2^60, far beyond 32 bits.
    EXPECT_EQ(harmonia::core::blockCount(kBc7, 0xFFFFFFFFU, 0xFFFFFFFFU), 1ULL << 60U);
}

// ── fullMipChainLevels ─────────────────────────────────────────────────────────

TEST(FullMipChainLevels, CountsDownToOneTexelOnTheLongestSide) {
    EXPECT_EQ(fullMipChainLevels(VkExtent2D{1U, 1U}), 1U);
    EXPECT_EQ(fullMipChainLevels(VkExtent2D{16U, 16U}), 5U);
    // 17 is not a power of two: 17, 8, 4, 2, 1.
    EXPECT_EQ(fullMipChainLevels(VkExtent2D{17U, 9U}), 5U);
    // The longest side decides.
    EXPECT_EQ(fullMipChainLevels(VkExtent2D{1U, 16U}), 5U);
    EXPECT_EQ(fullMipChainLevels(VkExtent2D{4096U, 4096U}), 13U);
    EXPECT_EQ(fullMipChainLevels(VkExtent2D{1920U, 1080U}), 11U);
    // A degenerate extent has no level below the first.
    EXPECT_EQ(fullMipChainLevels(VkExtent2D{0U, 0U}), 1U);
}

// ── mipChainFormatSupported ────────────────────────────────────────────────────

namespace {

harmonia::core::HostImageUploadDesc nearestDesc() {
    harmonia::core::HostImageUploadDesc desc{};
    desc.sampler.magFilter = VK_FILTER_NEAREST;
    desc.sampler.minFilter = VK_FILTER_NEAREST;
    desc.sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    return desc;
}

constexpr VkFormatFeatureFlags kSampled = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
constexpr VkFormatFeatureFlags kLinear = VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
#ifdef VK_FORMAT_FEATURE_2_HOST_IMAGE_TRANSFER_BIT
constexpr VkFormatFeatureFlags2 kHostTransfer = VK_FORMAT_FEATURE_2_HOST_IMAGE_TRANSFER_BIT;
#else
constexpr VkFormatFeatureFlags2 kHostTransfer = 0x400000000000ULL; // the bit's value once promoted
#endif

bool supportsHostTransferBit() {
#ifdef VK_FORMAT_FEATURE_2_HOST_IMAGE_TRANSFER_BIT
    return true;
#else
    return false;
#endif
}

} // namespace

// Negative control for an absent format feature: a format that reports nothing is
// refused, as is one that can be sampled but not written by a host copy.
TEST(MipChainFormatSupported, RefusesAFormatWithoutTheNeededFeatures) {
    const auto desc = nearestDesc();
    EXPECT_FALSE(mipChainFormatSupported(0U, 0U, desc));
    EXPECT_FALSE(mipChainFormatSupported(kLinear, kHostTransfer, desc)); // not sampleable
    if (supportsHostTransferBit()) {
        EXPECT_FALSE(mipChainFormatSupported(kSampled, 0U, desc)); // no host transfer
    }
}

TEST(MipChainFormatSupported, AcceptsASampleableFormatWithHostTransfer) {
    const auto desc = nearestDesc();
    EXPECT_TRUE(mipChainFormatSupported(kSampled, kHostTransfer, desc));
    EXPECT_TRUE(mipChainFormatSupported(kSampled | kLinear, kHostTransfer, desc));
    // Extra features do not hurt.
    EXPECT_TRUE(mipChainFormatSupported(kSampled | kLinear | VK_FORMAT_FEATURE_BLIT_SRC_BIT, ~0ULL, desc));
}

// The scene sampler filters linearly, so a chain for it needs the linear-filter feature.
TEST(MipChainFormatSupported, LinearFilteringNeedsTheLinearFeature) {
    harmonia::core::HostImageUploadDesc desc = nearestDesc();
    desc.sampler.minFilter = VK_FILTER_LINEAR;
    EXPECT_FALSE(mipChainFormatSupported(kSampled, kHostTransfer, desc));
    EXPECT_TRUE(mipChainFormatSupported(kSampled | kLinear, kHostTransfer, desc));

    harmonia::core::HostImageUploadDesc anisotropic = nearestDesc();
    anisotropic.sampler.anisotropyEnable = VK_TRUE;
    EXPECT_FALSE(mipChainFormatSupported(kSampled, kHostTransfer, anisotropic));
    EXPECT_TRUE(mipChainFormatSupported(kSampled | kLinear, kHostTransfer, anisotropic));
}

// The default sampler of the descriptor is the linear scene sampler.
TEST(MipChainFormatSupported, TheDefaultSamplerNeedsLinearFiltering) {
    const harmonia::core::HostImageUploadDesc desc{};
    EXPECT_FALSE(mipChainFormatSupported(kSampled, kHostTransfer, desc));
    EXPECT_TRUE(mipChainFormatSupported(kSampled | kLinear, kHostTransfer, desc));
}
