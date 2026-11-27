#ifndef HARMONIA_PIPELINE_GLAREPASS_HPP
#define HARMONIA_PIPELINE_GLAREPASS_HPP

#include <volk/volk.h>

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/Image.hpp"
#include "harmonia/core/VulkanHandle.hpp"
#include "harmonia/pipeline/IRenderPass.hpp"

namespace harmonia::pipeline {

struct PassContext;

/// Lens glare: the light a lens scatters from the bright parts of the frame.
///
/// Runs after the accumulation and the denoiser, on the image the tone mapper
/// reads. The frame is downsampled with a 13-tap filter into a chain of levels
/// (level 0, at half resolution, keeps only what lies above a soft knee around
/// the threshold) and upsampled again with a tent filter that adds every
/// coarser level to the one above it. The result is the glare texture, at
/// half resolution and in the frame's format, which the tone mapper adds to
/// the frame before its curve.
///
/// A presentation stage: the scene-referred capture (EXR) is written before
/// it and does not contain it.
class GlarePass final : public IRenderPass {
  public:
    /// The largest number of levels; a level is created while both of its
    /// dimensions are at least 2.
    static constexpr std::uint32_t kMaxLevels = 6U;

    struct Config {
        /// Scene-linear luminance above which the light of a texel is scattered.
        /// 1.0 is the frame's reference white. Below the threshold lies a soft
        /// knee of half its width, where the contribution rises from zero.
        float threshold = 1.0F;
        /// Share of each coarser level in the level above it, within 0..1.
        float levelWeight = 0.8F;
    };

    [[nodiscard]] static std::expected<GlarePass, VkResult> create(const DeviceContext& ctx,
                                                                   VkExtent2D extent,
                                                                   const std::filesystem::path& shaderDirectory,
                                                                   const Config& config);

    /// As above, with the default configuration.
    [[nodiscard]] static std::expected<GlarePass, VkResult>
    create(const DeviceContext& ctx, VkExtent2D extent, const std::filesystem::path& shaderDirectory);

    GlarePass() = default;
    GlarePass(const GlarePass&) = delete;
    GlarePass& operator=(const GlarePass&) = delete;
    GlarePass(GlarePass&&) noexcept = default;
    GlarePass& operator=(GlarePass&&) noexcept = default;
    ~GlarePass() noexcept override = default;

    /// Records the glare of the frame the tone mapper maps: the denoiser's
    /// output when the denoiser ran, else the accumulated HDR image.
    void record(const PassContext& ctx) noexcept override;
    /// The form for a caller that knows the image's last writers: `source` is
    /// in GENERAL, and its writes are covered by srcStage and srcAccess.
    void record(VkCommandBuffer cmd,
                const core::Image& source,
                VkPipelineStageFlags2 srcStage,
                VkAccessFlags2 srcAccess) noexcept;
    void onResize(VkExtent2D extent) noexcept override;
    [[nodiscard]] const char* name() const noexcept override { return "GlarePass"; }

    /// The glare of the last recorded frame, at half resolution and in the
    /// frame's format (32-bit float), in SHADER_READ_ONLY_OPTIMAL: what the
    /// tone mapper adds to the frame. Its texture coordinates are those of
    /// the frame.
    [[nodiscard]] VkImageView glareView() const noexcept { return m_upsampled[0].view(); }
    /// That image, for a read-back: 32-bit float RGBA, in
    /// SHADER_READ_ONLY_OPTIMAL after a record().
    [[nodiscard]] const core::Image& glareImage() const noexcept { return m_upsampled[0]; }
    [[nodiscard]] bool isValid() const noexcept { return m_pipelinePrefilter != VK_NULL_HANDLE && m_levelCount > 0U; }
    [[nodiscard]] const Config& config() const noexcept { return m_config; }
    /// The number of levels that exist for the current extent.
    [[nodiscard]] std::uint32_t levelCount() const noexcept { return m_levelCount; }

  private:
    /// One dispatch of the chain.
    struct Step {
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkImageView source = VK_NULL_HANDLE; ///< in GENERAL
        VkImageView coarse = VK_NULL_HANDLE; ///< in GENERAL; the upsample only
        VkImageView target = VK_NULL_HANDLE; ///< in GENERAL
        VkExtent2D sourceExtent{};
        VkExtent2D targetExtent{};
        float threshold = 0.0F; ///< the prefilter only
        float weight = 0.0F;    ///< the upsample only: the share of the coarse copy
    };

    [[nodiscard]] bool createPipelines(const std::filesystem::path& shaderDirectory);
    [[nodiscard]] bool createImages(VkExtent2D extent);
    void destroyImages() noexcept;
    void dispatch(VkCommandBuffer cmd, const Step& step) const noexcept;

    const DeviceContext* m_ctx = nullptr;
    core::UniquePipelineLayout m_pipelineLayout;
    core::UniqueDescriptorSetLayout m_setLayout;
    core::UniquePipeline m_pipelinePrefilter;
    core::UniquePipeline m_pipelineDownsample;
    core::UniquePipeline m_pipelineUpsample;
    Config m_config;
    VkExtent2D m_extent{};
    std::uint32_t m_levelCount = 0U;
    std::array<VkExtent2D, kMaxLevels> m_levelExtents{};
    std::array<core::Image, kMaxLevels> m_downsampled{}; ///< level i of the downsampled chain
    std::array<core::Image, kMaxLevels> m_upsampled{};   ///< level i of the upsampled chain
};

} // namespace harmonia::pipeline
#endif // HARMONIA_PIPELINE_GLAREPASS_HPP
