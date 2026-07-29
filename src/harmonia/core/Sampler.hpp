#ifndef HARMONIA_CORE_SAMPLER_HPP
#define HARMONIA_CORE_SAMPLER_HPP

#include <volk/volk.h>

namespace harmonia {

struct SamplerSpec {
    VkFilter magFilter = VK_FILTER_LINEAR;
    VkFilter minFilter = VK_FILTER_LINEAR;
    VkSamplerMipmapMode mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    VkSamplerAddressMode addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSamplerAddressMode addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSamplerAddressMode addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkBool32 anisotropyEnable = VK_FALSE;
    float maxAnisotropy = 1.0F;
    VkBorderColor borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
};

[[nodiscard]] inline VkSamplerCreateInfo makeSamplerCreateInfo(const SamplerSpec& spec) noexcept {
    return VkSamplerCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0U,
        .magFilter = spec.magFilter,
        .minFilter = spec.minFilter,
        .mipmapMode = spec.mipmapMode,
        .addressModeU = spec.addressModeU,
        .addressModeV = spec.addressModeV,
        .addressModeW = spec.addressModeW,
        .mipLodBias = 0.0F,
        .anisotropyEnable = spec.anisotropyEnable,
        .maxAnisotropy = spec.maxAnisotropy,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .minLod = 0.0F,
        .maxLod = 0.0F,
        .borderColor = spec.borderColor,
        .unnormalizedCoordinates = VK_FALSE,
    };
}

} // namespace harmonia

#endif // HARMONIA_CORE_SAMPLER_HPP
