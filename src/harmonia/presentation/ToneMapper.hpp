#ifndef HARMONIA_PRESENTATION_TONEMAPPER_HPP
#define HARMONIA_PRESENTATION_TONEMAPPER_HPP

#include <volk/volk.h>

#include <expected>
#include <filesystem>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/presentation/OutputColorSpace.hpp"
#include "harmonia/utils/ColorSpace.hpp"

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
    ToneMapper(ToneMapper&& other) noexcept;
    ToneMapper& operator=(ToneMapper&& other) noexcept;
    ~ToneMapper();

    /// Record the tone-mapping draw into cmd.
    /// The swapchain image must already be in VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL.
    /// colorSpace must match the swapchain's active OutputColorSpace.
    /// workingSpace is the scene-referred space of hdrView's contents — the
    /// tone mapper is the glue converting it to the display-referred output.
    void record(VkCommandBuffer cmd,
                VkImageView hdrView,
                VkImageView swapchainView,
                VkExtent2D extent,
                OutputColorSpace colorSpace,
                ColorSpace::WorkingColorSpace workingSpace = ColorSpace::WorkingColorSpace::LinRec2020) const noexcept;

    /// Overload that also selects the SDR/Display-P3 tone-mapping operator
    /// (matches tonemap.slang: 0=ACES, 1=AgX, 2=Reinhard, 3=Hable). Used by
    /// renderers that perform tone mapping in the present pass (e.g. Theia's
    /// forward renderer); offline renderers that tone-map earlier use the
    /// overload above and leave PushConstants::tonemapper untouched.
    void record(VkCommandBuffer cmd,
                VkImageView hdrView,
                VkImageView swapchainView,
                VkExtent2D extent,
                OutputColorSpace colorSpace,
                std::uint32_t tonemapper,
                ColorSpace::WorkingColorSpace workingSpace = ColorSpace::WorkingColorSpace::LinRec2020) const noexcept;

    [[nodiscard]] bool isValid() const noexcept { return m_pipeline != VK_NULL_HANDLE; }

  private:
    void destroy() noexcept;

    VkDevice m_device{};
    VkPipeline m_pipeline{};
    VkPipelineLayout m_pipelineLayout{};
    VkDescriptorSetLayout m_setLayout{}; ///< Push descriptor set layout (must outlive all CBs using it).
    VkFormat m_attachmentFormat{VK_FORMAT_UNDEFINED};
};
#endif // HARMONIA_PRESENTATION_TONEMAPPER_HPP
