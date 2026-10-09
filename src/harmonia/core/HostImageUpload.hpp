#ifndef HARMONIA_CORE_HOSTIMAGEUPLOAD_HPP
#define HARMONIA_CORE_HOSTIMAGEUPLOAD_HPP

#include <volk/volk.h>

#include <cstdint>
#include <expected>
#include <string_view>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/Image.hpp"
#include "harmonia/core/Sampler.hpp"
#include "harmonia/core/VulkanHandle.hpp"

namespace harmonia {

class CommandPool;

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
};

/// Image + sampler pair produced by uploadHostImage.
struct HostImageUpload {
    Image image;
    UniqueSampler sampler;
};

/// Vulkan 1.4 hostImageCopy upload + sampler creation, shared by Texture::create and
/// IblProbe::uploadEnvPanorama: stream host pixels straight into the (optimal-tiling)
/// image with vkCopyMemoryToImage — no staging buffer, no device-side copy. Layout path:
/// UNDEFINED -> GENERAL (queue), host copy into GENERAL, GENERAL -> SHADER_READ_ONLY (queue).
[[nodiscard]] std::expected<HostImageUpload, VkResult> uploadHostImage(const DeviceContext& ctx,
                                                                      const CommandPool& cmdPool,
                                                                      const HostImageUploadDesc& desc);

} // namespace harmonia

#endif // HARMONIA_CORE_HOSTIMAGEUPLOAD_HPP
