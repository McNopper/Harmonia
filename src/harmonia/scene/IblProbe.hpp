#pragma once

#include <volk/volk.h>

#include <glm/glm.hpp>

#include <expected>
#include <filesystem>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/Buffer.hpp"
#include "harmonia/core/CommandPool.hpp"
#include "harmonia/core/Image.hpp"
#include "harmonia/utils/ColorSpace.hpp"

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
    IblProbe(IblProbe&& other) noexcept;
    IblProbe& operator=(IblProbe&& other) noexcept;
    ~IblProbe();

    /// Load an equirectangular EXR panorama, convert to the working color
    /// space, and upload to GPU. Source primaries come from the EXR
    /// `chromaticities` attribute (fallback: linear Rec.709).
    /// Requires HARMONIA_HAS_OPENEXR; returns VK_ERROR_FEATURE_NOT_PRESENT otherwise.
    [[nodiscard]] static std::expected<IblProbe, VkResult>
    loadFromEXR(const DeviceContext& ctx,
                const CommandPool& pool,
                const std::filesystem::path& path,
                ColorSpace::WorkingColorSpace workingSpace = ColorSpace::WorkingColorSpace::LinRec2020);

    [[nodiscard]] VkImageView imageView() const noexcept { return m_image.view(); }
    [[nodiscard]] VkSampler sampler() const noexcept { return m_sampler; }
    [[nodiscard]] bool isValid() const noexcept { return m_sampler != VK_NULL_HANDLE; }

    /// CDF buffers for 2D env importance sampling (valid when cdfWidth() > 0).
    [[nodiscard]] const Buffer& marginalCdfBuffer() const noexcept { return m_marginalCdf; }
    [[nodiscard]] const Buffer& conditionalCdfBuffer() const noexcept { return m_conditionalCdf; }
    [[nodiscard]] uint32_t cdfWidth() const noexcept { return m_cdfWidth; }
    [[nodiscard]] uint32_t cdfHeight() const noexcept { return m_cdfHeight; }

    /// Dominant ("sun") direction extracted from the brightest region of the panorama,
    /// expressed as a normalised world-space direction pointing *towards* the light.
    /// Used to drive ray-traced directional shadows for IBL-lit scenes.
    [[nodiscard]] glm::vec3 sunDirection() const noexcept { return m_sunDirection; }

    /// Relative strength of the dominant light in [0,1]: 0 for a uniform/overcast
    /// panorama (no crisp shadows), approaching 1 for a clear, concentrated sun.
    [[nodiscard]] float sunStrength() const noexcept { return m_sunStrength; }

  private:
    void reset() noexcept;

    Image m_image{};
    VkSampler m_sampler{VK_NULL_HANDLE};
    const DeviceContext* m_ctx{};

    Buffer m_marginalCdf{};    ///< (H+1) floats: normalised marginal CDF over rows
    Buffer m_conditionalCdf{}; ///< H*(W+1) floats: normalised conditional CDF per row
    uint32_t m_cdfWidth{0};
    uint32_t m_cdfHeight{0};

    glm::vec3 m_sunDirection{0.0f, 1.0f, 0.0f}; ///< world-space direction towards the dominant light
    float m_sunStrength{0.0f};                  ///< [0,1] concentration of the dominant light
};
