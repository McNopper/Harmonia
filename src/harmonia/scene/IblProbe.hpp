#ifndef HARMONIA_SCENE_IBLPROBE_HPP
#define HARMONIA_SCENE_IBLPROBE_HPP

#include <volk/volk.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <slang-math/slang-math.hpp>
#include <vector>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/Buffer.hpp"
#include "harmonia/core/CommandPool.hpp"
#include "harmonia/core/Image.hpp"
#include "harmonia/core/VulkanHandle.hpp"
#include "harmonia/utils/ColorSpace.hpp"

namespace harmonia {

/// Image-based lighting probe loaded from an equirectangular HDR panorama (EXR).
///
/// The panorama is stored on the GPU as a 2D RGBA32F texture in the scene's
/// (linear) working color space. The source primaries are read from the EXR
/// `chromaticities` header attribute (Rec.709 or Rec.2020); when absent,
/// linear Rec.709 is assumed (the OpenEXR default).
///
/// A 2D separable CDF (256×128) is also built from the panorama luminance for
/// environment map importance sampling (env NEE + MIS).
/// Ref: Pharr, Jakob & Humphreys — "Physically Based Rendering" 4th ed. §12.5 & §13.4.3
class IblProbe {
  public:
    IblProbe() = default;
    IblProbe(const IblProbe&) = delete;
    IblProbe& operator=(const IblProbe&) = delete;
    IblProbe(IblProbe&&) noexcept = default;
    IblProbe& operator=(IblProbe&&) noexcept = default;
    ~IblProbe() noexcept = default;

    /// Load an equirectangular EXR panorama, convert to the working color
    /// space, and upload to GPU. Source primaries come from the EXR
    /// `chromaticities` attribute (fallback: linear Rec.709).
    /// Requires HARMONIA_HAS_OPENEXR; returns VK_ERROR_FEATURE_NOT_PRESENT otherwise.
    [[nodiscard]] static std::expected<IblProbe, VkResult> loadFromEXR(
        const DeviceContext& ctx,
        const CommandPool& pool,
        const std::filesystem::path& path,
        harmonia::ColorSpace::WorkingColorSpace workingSpace = harmonia::ColorSpace::WorkingColorSpace::LinRec2020);

    [[nodiscard]] VkImageView imageView() const noexcept { return m_image.view(); }
    [[nodiscard]] VkSampler sampler() const noexcept { return m_sampler; }
    [[nodiscard]] bool isValid() const noexcept { return m_sampler != VK_NULL_HANDLE; }

    /// CDF buffers for 2D env importance sampling (valid when cdfWidth() > 0).
    [[nodiscard]] const Buffer& marginalCdfBuffer() const noexcept { return m_marginalCdf; }
    [[nodiscard]] const Buffer& conditionalCdfBuffer() const noexcept { return m_conditionalCdf; }
    [[nodiscard]] std::uint32_t cdfWidth() const noexcept { return m_cdfWidth; }
    [[nodiscard]] std::uint32_t cdfHeight() const noexcept { return m_cdfHeight; }

    /// Dominant ("sun") direction extracted from the brightest region of the panorama,
    /// expressed as a normalised world-space direction pointing *towards* the light.
    /// Used to drive ray-traced directional shadows for IBL-lit scenes.
    [[nodiscard]] sm::float3 sunDirection() const noexcept { return m_sunDirection; }

    /// Relative strength of the dominant light in [0,1]: 0 for a uniform/overcast
    /// panorama (no crisp shadows), approaching 1 for a clear, concentrated sun.
    [[nodiscard]] float sunStrength() const noexcept { return m_sunStrength; }

  private:
    struct ExrData {
        std::size_t width = 0;
        std::size_t height = 0;
        std::vector<float> raw;
        bool srcRec2020 = false;
    };

    [[nodiscard]] static std::expected<ExrData, VkResult> readEXR(const std::filesystem::path& path);
    [[nodiscard]] static std::vector<float> convertPrimaries(const std::vector<float>& raw,
                                                             std::size_t width,
                                                             std::size_t height,
                                                             bool srcRec2020,
                                                             harmonia::ColorSpace::WorkingColorSpace workingSpace);
    [[nodiscard]] static VkResult uploadEnvPanorama(IblProbe& probe,
                                                    const DeviceContext& ctx,
                                                    const CommandPool& pool,
                                                    const std::vector<float>& rgba32f,
                                                    std::size_t width,
                                                    std::size_t height);
    static void buildImportanceCdf(IblProbe& probe,
                                   const DeviceContext& ctx,
                                   const std::vector<float>& rgba32f,
                                   std::size_t width,
                                   std::size_t height);
    static void extractDominantSun(IblProbe& probe,
                                   float sunBestAvg,
                                   std::size_t sunBestU,
                                   std::size_t sunBestV,
                                   double sunAvgSum,
                                   std::size_t sunAvgCount);

    Image m_image{};
    harmonia::UniqueSampler m_sampler;

    Buffer m_marginalCdf{};    ///< (H+1) floats: normalised marginal CDF over rows
    Buffer m_conditionalCdf{}; ///< H*(W+1) floats: normalised conditional CDF per row
    std::uint32_t m_cdfWidth{0};
    std::uint32_t m_cdfHeight{0};

    sm::float3 m_sunDirection{0.0f, 1.0f, 0.0f}; ///< world-space direction towards the dominant light
    float m_sunStrength{0.0f};                   ///< [0,1] concentration of the dominant light
};

} // namespace harmonia

#endif // HARMONIA_SCENE_IBLPROBE_HPP
