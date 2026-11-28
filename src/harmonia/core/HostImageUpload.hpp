#ifndef HARMONIA_CORE_HOSTIMAGEUPLOAD_HPP
#define HARMONIA_CORE_HOSTIMAGEUPLOAD_HPP

#include <volk/volk.h>

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/Image.hpp"
#include "harmonia/core/Sampler.hpp"
#include "harmonia/core/VulkanHandle.hpp"

namespace harmonia::core {

class CommandPool;

/// Texel block footprint of a VkFormat: the block extent in texels and the size of one
/// block in bytes. Block-compressed formats (BC1..BC7) have a 4x4 extent; a format of the
/// table that is not block-compressed is one texel per block. A format outside the table
/// (the BC formats and the uncompressed formats this helper uploads) returns {0, 0, 0}.
struct TexelBlock {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t bytes{};

    [[nodiscard]] constexpr bool operator==(const TexelBlock&) const noexcept = default;
};

/// Single source of truth for the host-layout size of one image level.
[[nodiscard]] TexelBlock texelBlock(VkFormat format) noexcept;

/// Number of texel blocks that cover a level of @p extent; a partial block at the right or
/// bottom edge is stored as a whole block.
[[nodiscard]] constexpr std::uint64_t
blockCount(const TexelBlock& block, std::uint32_t width, std::uint32_t height) noexcept {
    return ((static_cast<std::uint64_t>(width) + block.width - 1U) / block.width) *
           ((static_cast<std::uint64_t>(height) + block.height - 1U) / block.height);
}

/// Number of levels of the complete mip chain of @p extent.
[[nodiscard]] constexpr std::uint32_t fullMipChainLevels(VkExtent2D extent) noexcept {
    std::uint32_t levels = 1U;
    std::uint32_t longest = extent.width > extent.height ? extent.width : extent.height;
    while (longest > 1U) {
        longest >>= 1U;
        ++levels;
    }
    return levels;
}

/// One level of a caller-supplied mip chain.
struct HostImageMipLevel {
    const void* data = nullptr;   ///< first byte of the level in host memory
    std::uint64_t byteLength = 0; ///< bytes readable from data; must cover the whole level
    std::uint32_t width = 0;      ///< level extent in texels
    std::uint32_t height = 0;
};

/// Configuration for uploadHostImage (parameter object).
struct HostImageUploadDesc {
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    const void* pixels = nullptr;
    VkExtent2D extent{0, 0};
    /// dst stage/access of the final GENERAL -> SHADER_READ_ONLY_OPTIMAL transition.
    /// Barrier scope is load-bearing — pass exactly the stages that sample the image.
    VkPipelineStageFlags2 finalStages = 0;
    VkAccessFlags2 finalAccess = 0;
    /// Image debug name.
    std::string_view name;
    /// Sampler configuration for the created sampler.
    SamplerSpec sampler{};
    /// Precomputed mip chain, one entry per level starting at level 0 (level i covers
    /// max(1, extent >> i)). Non-empty: the image gets exactly mips.size() levels, every
    /// level is written with its own vkCopyMemoryToImage region, nothing is generated (no
    /// blit, no TRANSFER usage) and `pixels` is ignored. Empty: `pixels` is uploaded as
    /// level 0 and the chain below it is generated on the GPU when the format blits.
    std::span<const HostImageMipLevel> mips{};
};

/// Image + sampler pair produced by uploadHostImage.
struct HostImageUpload {
    Image image;
    UniqueSampler sampler;
};

/// Format support a precomputed-level upload (HostImageUploadDesc::mips) needs from its
/// format with optimal tiling: the image is sampled with the desc's sampler and written by
/// vkCopyMemoryToImage. @p optimalTiling and @p optimalTiling2 are
/// VkFormatProperties::optimalTilingFeatures and VkFormatProperties3::optimalTilingFeatures.
[[nodiscard]] constexpr bool mipChainFormatSupported(VkFormatFeatureFlags optimalTiling,
                                                     VkFormatFeatureFlags2 optimalTiling2,
                                                     const HostImageUploadDesc& desc) noexcept {
    VkFormatFeatureFlags required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if (desc.sampler.magFilter == VK_FILTER_LINEAR || desc.sampler.minFilter == VK_FILTER_LINEAR ||
        desc.sampler.anisotropyEnable != VK_FALSE) {
        required |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    }
    if ((optimalTiling & required) != required) {
        return false;
    }
    // VUID-VkImageCreateInfo-imageCreateFormatFeatures-09048: HOST_TRANSFER usage needs the bit.
#ifdef VK_FORMAT_FEATURE_2_HOST_IMAGE_TRANSFER_BIT
    return (optimalTiling2 & VK_FORMAT_FEATURE_2_HOST_IMAGE_TRANSFER_BIT) != 0;
#else
    // Headers of an SDK that predates the promotion of the bit to Vulkan 1.4 have neither
    // the bit nor the rule; the spec guarantees it for every sampled colour format.
    static_cast<void>(optimalTiling2);
    return true;
#endif
}

/// Vulkan 1.4 hostImageCopy upload + sampler creation, shared by scene::Texture::create and
/// scene::IblProbe::uploadEnvPanorama: stream host pixels straight into the (optimal-tiling)
/// image with vkCopyMemoryToImage — no staging buffer, no device-side copy. Layout path:
/// UNDEFINED -> GENERAL (queue), host copy into GENERAL, GENERAL -> SHADER_READ_ONLY (queue).
/// A precomputed chain (HostImageUploadDesc::mips) takes the same layout path with one copy
/// region per level and no level generation.
[[nodiscard]] std::expected<HostImageUpload, VkResult>
uploadHostImage(const DeviceContext& ctx, const CommandPool& cmdPool, const HostImageUploadDesc& desc);

} // namespace harmonia::core

#endif // HARMONIA_CORE_HOSTIMAGEUPLOAD_HPP
