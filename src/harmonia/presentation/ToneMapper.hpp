#ifndef HARMONIA_PRESENTATION_TONEMAPPER_HPP
#define HARMONIA_PRESENTATION_TONEMAPPER_HPP

#include <volk/volk.h>

#include <cstdint>
#include <expected>
#include <filesystem>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/VulkanHandle.hpp"
#include "harmonia/utils/ColorSpace.hpp"
#include "harmonia/utils/OutputColorSpace.hpp"

namespace harmonia::presentation {

class ToneMapper {
  public:
    /// Create a graphics-pipeline tone mapper using dynamic rendering.
    /// swapchainFormat must match the current swapchain image format.
    /// The ToneMapper owns its own pipeline layout; no external layout is needed.
    [[nodiscard]] static std::expected<ToneMapper, VkResult> create(const DeviceContext& ctx,
                                                                    VkFormat swapchainFormat,
                                                                    const std::filesystem::path& vertSpvPath,
                                                                    const std::filesystem::path& fragSpvPath);

    ToneMapper() = default;
    ToneMapper(const ToneMapper&) = delete;
    ToneMapper& operator=(const ToneMapper&) = delete;
    ToneMapper(ToneMapper&&) noexcept = default;
    ToneMapper& operator=(ToneMapper&&) noexcept = default;
    ~ToneMapper() noexcept = default;

    /// The lens glare a draw adds to the frame before its tone curve.
    struct GlareInput {
        /// The result of pipeline::GlarePass: in SHADER_READ_ONLY_OPTIMAL, and
        /// at half the extent of the frame. A null view adds no glare.
        VkImageView view = VK_NULL_HANDLE;
        /// The share of the glare that is added. 0 adds no glare.
        float strength = 0.0F;
    };

    /// Record the tone-mapping draw into cmd.
    /// The swapchain image must already be in VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL.
    /// colorSpace must match the swapchain's active utils::OutputColorSpace.
    /// workingSpace is the scene-referred space of hdrView's contents — the
    /// tone mapper is the glue converting it to the display-referred output.
    /// The frame's image is only read: the glare is in the output, not in the
    /// image, so a capture of that image is the frame without glare.
    void record(VkCommandBuffer cmd,
                VkImageView hdrView,
                VkImageView swapchainView,
                VkExtent2D extent,
                utils::OutputColorSpace colorSpace,
                harmonia::utils::ColorSpace::WorkingColorSpace workingSpace =
                    harmonia::utils::ColorSpace::WorkingColorSpace::LinRec2020) const noexcept;

    /// As above, with the lens glare added to the frame before the tone curve.
    void record(VkCommandBuffer cmd,
                VkImageView hdrView,
                VkImageView swapchainView,
                VkExtent2D extent,
                utils::OutputColorSpace colorSpace,
                harmonia::utils::ColorSpace::WorkingColorSpace workingSpace,
                const GlareInput& glare) const noexcept;

    /// Overload that also selects the SDR/Display-P3 tone-mapping operator
    /// (matches tonemap.slang: 0=ACES, 1=AgX, 2=Reinhard, 3=Hable). Used by
    /// renderers that perform tone mapping in the present pass (e.g. Theia's
    /// forward renderer); offline renderers that tone-map earlier use the
    /// overload above and leave PushConstants::tonemapper untouched.
    void record(VkCommandBuffer cmd,
                VkImageView hdrView,
                VkImageView swapchainView,
                VkExtent2D extent,
                utils::OutputColorSpace colorSpace,
                std::uint32_t tonemapper,
                harmonia::utils::ColorSpace::WorkingColorSpace workingSpace =
                    harmonia::utils::ColorSpace::WorkingColorSpace::LinRec2020) const noexcept;

    /// As above, with the lens glare added to the frame before the tone curve.
    void record(VkCommandBuffer cmd,
                VkImageView hdrView,
                VkImageView swapchainView,
                VkExtent2D extent,
                utils::OutputColorSpace colorSpace,
                std::uint32_t tonemapper,
                harmonia::utils::ColorSpace::WorkingColorSpace workingSpace,
                const GlareInput& glare) const noexcept;

    [[nodiscard]] bool isValid() const noexcept { return m_pipeline != VK_NULL_HANDLE; }

  private:
    harmonia::core::UniquePipeline m_pipeline;
    harmonia::core::UniquePipelineLayout m_pipelineLayout;
    harmonia::core::UniqueDescriptorSetLayout
        m_setLayout; ///< Push descriptor set layout (must outlive all CBs using it).
    VkFormat m_attachmentFormat{VK_FORMAT_UNDEFINED};
};

} // namespace harmonia::presentation

#endif // HARMONIA_PRESENTATION_TONEMAPPER_HPP
