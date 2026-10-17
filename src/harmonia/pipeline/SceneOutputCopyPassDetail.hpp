#ifndef HARMONIA_PIPELINE_SCENEOUTPUTCOPYPASSDETAIL_HPP
#define HARMONIA_PIPELINE_SCENEOUTPUTCOPYPASSDETAIL_HPP

// Detail shared between the SceneOutputCopyPass translation units
// (SceneOutputCopyPass.cpp + SceneOutputCopyPassDenoise.cpp).
// Not public API - do not include from outside the pass.

#include <algorithm>
#include <cstdint>

namespace harmonia::pipeline {

struct alignas(16) DenoiserPushConstants {
    float strength = 0.45F;
    float historyBlend = 0.15F;
    std::uint32_t hasNormalGuide = 0U;
    std::uint32_t hasDepthGuide = 0U;
    std::uint32_t applyHistory = 0U;
    std::uint32_t historyFirstUse = 1U;
    std::uint32_t iterations = 1U;
    std::uint32_t passIndex = 0U;
    std::uint32_t hasMotionVectors = 0U; ///< 1 when ctx.motionVectorView is populated
    // ── A-SVGF (A2) ───────────────────────────────────────────────────────────
    std::uint32_t computeGradient = 0U;     ///< temporal pass computes gradient + variance
    std::uint32_t gradientFilterMode = 0U;  ///< 1 = this dispatch is the gradient à-trous blur pass
    std::uint32_t gradientFilterPass = 0U;  ///< gradient à-trous pass index (tap spacing)
    std::uint32_t hasGradientVariance = 0U; ///< gGradientVariance holds valid gradient/variance
    float gradientAlpha = 0.2F;             ///< temporal blend factor for the gradient
};

[[nodiscard]] inline float clamp01(float value) noexcept {
    return std::clamp(value, 0.0F, 1.0F);
}

[[nodiscard]] inline std::uint32_t clampIterations(std::uint32_t iterations) noexcept {
    return std::clamp(iterations, 1U, 8U);
}

} // namespace harmonia::pipeline

#endif // HARMONIA_PIPELINE_SCENEOUTPUTCOPYPASSDETAIL_HPP
