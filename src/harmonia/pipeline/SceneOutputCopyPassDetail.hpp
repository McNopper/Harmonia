#ifndef HARMONIA_PIPELINE_SCENEOUTPUTCOPYPASSDETAIL_HPP
#define HARMONIA_PIPELINE_SCENEOUTPUTCOPYPASSDETAIL_HPP

// Detail shared between the SceneOutputCopyPass translation units
// (SceneOutputCopyPass.cpp + SceneOutputCopyPassDenoise.cpp).
// Not public API - do not include from outside the pass.

#include <algorithm>
#include <cstdint>
#include <slang-math/slang-math.hpp>

#include "harmonia/pipeline/DenoiserDefaults.hpp"

namespace harmonia::pipeline {

struct alignas(16) DenoiserPushConstants {
    float strength = kDenoiserStrength;
    float historyBlend = kDenoiserHistoryBlend;
    std::uint32_t hasNormalGuide = 0U;
    std::uint32_t hasDepthGuide = 0U;
    std::uint32_t applyHistory = 0U;
    std::uint32_t historyFirstUse = 1U;
    std::uint32_t iterations = 1U;
    std::uint32_t passIndex = 0U;
    std::uint32_t hasMotionVectors = 0U; ///< 1 when ctx.motionVectorView is populated
    // ── A-SVGF (A2) ───────────────────────────────────────────────────────────
    std::uint32_t computeGradient = 0U;           ///< temporal pass computes gradient + variance
    std::uint32_t gradientFilterMode = 0U;        ///< 1 = this dispatch is the gradient à-trous blur pass
    std::uint32_t gradientFilterPass = 0U;        ///< gradient à-trous pass index (tap spacing)
    std::uint32_t hasGradientVariance = 0U;       ///< gGradientVariance holds valid gradient/variance
    float gradientAlpha = kDenoiserGradientAlpha; ///< temporal blend factor for the gradient
    // ── DN3-M2: paper-exact converging blend (Eq. 2 / Eq. 14) ────────────────
    std::uint32_t convergingMode = 0U;  ///< 1 = Eq. 2 blend with the Eq. 14 per-pixel SURE weight
    std::uint32_t perturbedMode = 0U;   ///< 1 = this dispatch filters the perturbed input (Eq. 13 probe)
    std::uint32_t sampleCount = 0U;     ///< current accumulated sample count I
    std::uint32_t prevSampleCount = 0U; ///< previous frame's count (Eq. 7 curve second point)
};

[[nodiscard]] inline float clamp01(float value) noexcept {
    return sm::saturate(value);
}

[[nodiscard]] inline std::uint32_t clampIterations(std::uint32_t iterations) noexcept {
    return std::clamp(iterations, 1U, 8U);
}

} // namespace harmonia::pipeline

#endif // HARMONIA_PIPELINE_SCENEOUTPUTCOPYPASSDETAIL_HPP
