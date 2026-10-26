#ifndef HARMONIA_PIPELINE_DENOISERDEFAULTS_HPP
#define HARMONIA_PIPELINE_DENOISERDEFAULTS_HPP

#include <cstdint>

namespace harmonia::pipeline {

/// A-SVGF scene-output denoiser defaults - single source of truth.
/// AppConfig (user-facing config) and the denoiser push-constant block
/// mirror these values; change them here only.
inline constexpr float kDenoiserStrength = 0.45F;        ///< spatial denoiser strength [0, 1]
inline constexpr std::uint32_t kDenoiserIterations = 2U; ///< bilateral passes [1, 8]
inline constexpr float kDenoiserHistoryBlend = 0.15F;    ///< temporal blend factor [0, 1]
inline constexpr float kDenoiserGradientAlpha = 0.2F;    ///< A-SVGF gradient temporal blend [0, 1]

} // namespace harmonia::pipeline

#endif // HARMONIA_PIPELINE_DENOISERDEFAULTS_HPP
