#ifndef HARMONIA_PIPELINE_SCENEOUTPUTCOPYPASS_HPP
#define HARMONIA_PIPELINE_SCENEOUTPUTCOPYPASS_HPP

#include <volk/volk.h>

#include <cstdint>
#include <expected>
#include <filesystem>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/Image.hpp"
#include "harmonia/core/VulkanHandle.hpp"
#include "harmonia/pipeline/IRenderPass.hpp"

namespace harmonia {

struct PassContext;

/// Configuration for the denoiser/accumulation pass.
/// Defined outside SceneOutputCopyPass to avoid CWG2664 (Clang 22+ C++23 restriction on nested-struct
/// NSDMIs in default arguments / data-member initializers of the enclosing class).
struct SceneOutputCopyPassSettings {
    float strength = 0.45F;        ///< spatial denoiser strength [0, 1]
    std::uint32_t iterations = 2U; ///< bilateral passes [1, 8]
    bool useHistory = true;        ///< enable temporal blending in fixed-view mode
    float historyBlend = 0.15F;    ///< temporal blend factor [0, 1]
    bool useGradient = true;       ///< A-SVGF: gradient-driven adaptive history + variance-guided spatial filter
    float gradientAlpha = 0.2F;    ///< A-SVGF: temporal blend factor for the gradient [0, 1]
};

/// Shared denoiser stage operating on scene-referred HDR output before tone mapping.
class SceneOutputCopyPass final : public IRenderPass {
  public:
    /// \deprecated Use harmonia::SceneOutputCopyPassSettings directly.
    using Settings = SceneOutputCopyPassSettings;

    [[nodiscard]] static std::expected<SceneOutputCopyPass, VkResult>
    create(const DeviceContext& ctx,
           VkExtent2D extent,
           const std::filesystem::path& computeSpvPath,
           const Settings& settings = {});

    SceneOutputCopyPass() = default;
    SceneOutputCopyPass(const SceneOutputCopyPass&) = delete;
    SceneOutputCopyPass& operator=(const SceneOutputCopyPass&) = delete;
    SceneOutputCopyPass(SceneOutputCopyPass&&) noexcept = default;
    SceneOutputCopyPass& operator=(SceneOutputCopyPass&&) noexcept = default;
    ~SceneOutputCopyPass() noexcept override = default;

    void record(const PassContext& ctx) noexcept override;
    void onResize(VkExtent2D extent) noexcept override;
    [[nodiscard]] const char* name() const noexcept override { return "SceneDenoiserPass"; }

    /// A3(b): view of the A-SVGF per-pixel gradient/variance image (R32G32F,
    /// R = temporal gradient, G = variance). Valid after the first record() call;
    /// always in VK_IMAGE_LAYOUT_GENERAL. Returns VK_NULL_HANDLE when useGradient
    /// is false or the pass has not been initialized.
    [[nodiscard]] VkImageView gradientImageView() const noexcept {
        return (m_settings.useGradient && m_gradientImage.isValid()) ? m_gradientImage.view() : VK_NULL_HANDLE;
    }

  private:
    [[nodiscard]] bool createWorkImages(VkExtent2D extent) noexcept;
    void resetHistory(std::uint64_t resetToken) noexcept;

    void barrierForComputeRead(VkCommandBuffer cmd, VkImage hdrImage, VkImage denoisedImage, bool useGradient) noexcept;
    [[nodiscard]] VkImage recordSpatialPasses(VkCommandBuffer cmd,
                                              const Image& hdrBuffer,
                                              const Image& denoised,
                                              std::uint32_t iterations,
                                              std::uint32_t groupsX,
                                              std::uint32_t groupsY,
                                              VkImageView normalGuideView,
                                              VkImageView depthGuideView,
                                              bool hasNormalGuide,
                                              bool hasDepthGuide,
                                              VkImageView motionVecView,
                                              VkImageView gradientView,
                                              VkImageView prevGradientView,
                                              bool hasGradientVariance) noexcept;
    void recordTemporalHistoryPass(VkCommandBuffer cmd,
                                   const Image& denoised,
                                   bool useGradient,
                                   bool hasMotionVectors,
                                   std::uint32_t iterations,
                                   std::uint32_t groupsX,
                                   std::uint32_t groupsY,
                                   VkImageView normalGuideView,
                                   VkImageView depthGuideView,
                                   bool hasNormalGuide,
                                   bool hasDepthGuide,
                                   VkImageView motionVecView,
                                   VkImageView gradientView,
                                   VkImageView prevGradientView) noexcept;
    void recordGradientBlur(VkCommandBuffer cmd,
                            VkExtent2D extent,
                            std::uint32_t iterations,
                            std::uint32_t groupsX,
                            std::uint32_t groupsY) noexcept;
    void copyImageRoundTrip(VkCommandBuffer cmd,
                            VkImage srcImage,
                            VkImage dstImage,
                            VkExtent2D extent,
                            VkAccessFlags2 preSrcAccess,
                            VkPipelineStageFlags2 restoreStage) noexcept;
    void restoreBarriers(VkCommandBuffer cmd, VkImage hdrImage, VkImage denoisedImage) noexcept;

    const DeviceContext* m_ctx = nullptr;
    UniquePipeline m_pipeline;
    UniquePipelineLayout m_pipelineLayout;
    UniqueDescriptorSetLayout m_setLayout;
    UniqueSampler m_guideSampler;

    Image m_historyImage{};
    Image m_workImage{};
    Image m_dummyMotionVectors{}; ///< 1×1 R32G32F fallback bound to binding 5 when ctx.motionVectorView is null
    Image m_gradientImage{};      ///< A-SVGF R32G32F gradient/variance (binding 6); full-res, history lifetime
    Image m_prevGradientImage{};  ///< A-SVGF R32G32F previous gradient/variance (binding 7)
    Image m_dummyGradient{};      ///< 1×1 R32G32F fallback bound to bindings 6/7 when useGradient is off
    Settings m_settings;          // Default-initialized (uses NSDMIs from SceneOutputCopyPassSettings)
    VkExtent2D m_extent{};
    std::uint64_t m_lastResetToken = 0U;
    bool m_firstUse = true;
    bool m_historyFirstUse = true;
};

} // namespace harmonia
#endif // HARMONIA_PIPELINE_SCENEOUTPUTCOPYPASS_HPP
