#ifndef HARMONIA_UTILS_TONEMAPPING_HPP
#define HARMONIA_UTILS_TONEMAPPING_HPP

// CPU-side tone mapping functions mirroring shaders/tonemap.slang.
// Used for unit testing and offline processing; keeps shader and CPU logic in sync.
//
// All functions operate in linear Rec.2020.  Apply color-space conversions
// (rec2020ToRec709, etc.) and OETFs separately via ColorSpace.hpp.
//
// References:
//   ACES colour matrices — AMPAS ACES specification S-2014-003
//     https://docs.acescentral.com/specifications/acescg/
//   ACES RRT+ODT analytic fit — Stephen Hill (BakingLab ACES.hlsl)
//     https://github.com/TheRealMJP/BakingLab/blob/master/BakingLab/ACES.hlsl
//   AgX — Troy Sobotka; Blender / OCIO reference configuration
//   Hable/Uncharted-2 filmic — John Hable, GDC 2010
//   Reinhard et al. — SIGGRAPH 2002, "Photographic Tone Reproduction for Digital Images"

#include <algorithm>
#include <cmath>
#include <slang-math/slang-math.hpp>

#include "harmonia/utils/ColorSpace.hpp"

namespace harmonia::utils::ToneMapping {

// ── ACES matrices ─────────────────────────────────────────────────────────────
// Row-major slang-math form, matching ColorSpace.cpp kRec2020_to_AP1 / kAP1_to_Rec2020.
// Composed output matrices derived from those values (see tonemap.slang for row-major form).

// clang-format off
/// Rec.2020 → ACEScg AP1
inline constexpr sm::float3x3 kRec2020ToAP1 = sm::float3x3(
    {0.6131324f, 0.3395255f, 0.0474491f},  // row 0
    {0.0701243f, 0.9163394f, 0.0135363f},  // row 1
    {0.0205076f, 0.1096098f, 0.8699926f}   // row 2
);

/// ACEScg AP1 → Rec.709 D65  (composed: AP1 → Rec.2020 → Rec.709)
inline constexpr sm::float3x3 kAP1ToRec709 = sm::float3x3(
    { 2.9090f, -1.6933f, -0.2159f},  // row 0
    {-0.3595f,  1.3712f, -0.0114f},  // row 1
    {-0.0447f, -0.2477f,  1.2925f}   // row 2
);

/// ACEScg AP1 → Display P3 D65  (composed: AP1 → Rec.2020 → P3)
inline constexpr sm::float3x3 kAP1ToP3 = sm::float3x3(
    { 2.1173f, -1.0180f, -0.0996f},  // row 0
    {-0.2071f,  1.2150f, -0.0077f},  // row 1
    {-0.0219f, -0.1540f,  1.1758f}   // row 2
);
// clang-format on

// ── ACES RRT+ODT analytic fit (Stephen Hill, SIGGRAPH 2016) ─────────────────

/// ACES RRT+ODT rational-polynomial approximation.
/// Input/output: ACEScg AP1 linear.
[[nodiscard]] inline sm::float3 acesRrtOdtFit(sm::float3 v) noexcept {
    const sm::float3 a =
        v * (v + sm::float3{0.0245786f, 0.0245786f, 0.0245786f}) - sm::float3{0.000090537f, 0.000090537f, 0.000090537f};
    const sm::float3 b = v * (0.983729f * v + sm::float3{0.4329510f, 0.4329510f, 0.4329510f}) +
                         sm::float3{0.238081f, 0.238081f, 0.238081f};
    return a / b;
}

/// Full ACES tone mapper: Rec.2020 linear → tone-mapped in target primaries.
/// outputMat must be kAP1ToRec709 (SDR / scRGB) or kAP1ToP3 (Display P3).
[[nodiscard]] inline sm::float3 acesFitted(sm::float3 c2020, const sm::float3x3& outputMat) noexcept {
    sm::float3 ap1 = kRec2020ToAP1 * c2020;
    ap1 = sm::max(ap1, sm::float3{0.f, 0.f, 0.f});
    ap1 = acesRrtOdtFit(ap1);
    return sm::max(outputMat * ap1, sm::float3{0.f, 0.f, 0.f});
}

/// ACES for SDR (Rec.709 output).
[[nodiscard]] inline sm::float3 acesFittedSDR(sm::float3 c2020) noexcept {
    return acesFitted(c2020, kAP1ToRec709);
}

/// ACES for Display P3 output.
[[nodiscard]] inline sm::float3 acesFittedP3(sm::float3 c2020) noexcept {
    return acesFitted(c2020, kAP1ToP3);
}

// ── AgX (Troy Sobotka, 2022; Blender/OCIO matrices) ──────────────────────────
// clang-format off
inline constexpr sm::float3x3 kAgXInputMatrix = sm::float3x3(
    {0.842479f, 0.042328f, 0.042376f},
    {0.078434f, 0.878469f, 0.078434f},
    {0.079224f, 0.079166f, 0.879143f}
);
inline constexpr sm::float3x3 kAgXOutputMatrix = sm::float3x3(
    { 1.196879f, -0.052897f, -0.052972f},
    {-0.098021f,  1.151903f, -0.098043f},
    {-0.099030f, -0.098961f,  1.151074f}
);
// clang-format on

[[nodiscard]] inline float agxCurve(float x) noexcept {
    x = std::clamp(x, 0.0F, 1.0F);
    const float x2 = x * x;
    const float x3 = x2 * x;
    const float x4 = x2 * x2;
    const float x5 = x3 * x2;
    const float x6 = x3 * x3;
    return 15.5F * x6 - 40.14F * x5 + 31.96F * x4 - 6.868F * x3 + 0.4298F * x2 + 0.1191F * x - 0.00232F;
}

/// AgX Rec.2020 scene-linear -> Rec.709 display-linear (apply sRGB OETF separately).
[[nodiscard]] inline sm::float3 agxFittedSDR(sm::float3 c2020) noexcept {
    sm::float3 agx = sm::max(ColorSpace::rec2020ToRec709(c2020), sm::float3{0.0F, 0.0F, 0.0F});
    agx = kAgXInputMatrix * agx;
    const auto logEncode = [](float v) { return (std::log2(std::max(v, 2.2e-7F)) + 10.0F) / 16.5F; };
    agx = sm::float3{agxCurve(logEncode(agx.x)), agxCurve(logEncode(agx.y)), agxCurve(logEncode(agx.z))};
    return kAgXOutputMatrix * agx;
}

// ── Hable / Uncharted-2 (John Hable, GDC 2010) ───────────────────────────────

[[nodiscard]] inline sm::float3 hablePartial(sm::float3 x) noexcept {
    constexpr float A = 0.15f, B = 0.50f, C = 0.10f, D = 0.20f, E = 0.02f, F = 0.30f;
    return ((x * (A * x + sm::float3{C * B, C * B, C * B}) + sm::float3{D * E, D * E, D * E}) /
            (x * (A * x + sm::float3{B, B, B}) + sm::float3{D * F, D * F, D * F})) -
           sm::float3{E / F, E / F, E / F};
}

/// Hable/Uncharted-2 filmic tone mapper.
/// Output is in Rec.2020; apply rec2020ToRec709 + sRGB OETF for SDR display.
[[nodiscard]] inline sm::float3 hableFilmic(sm::float3 x) noexcept {
    const sm::float3 white = hablePartial(sm::float3{11.2f, 11.2f, 11.2f});
    return hablePartial(2.0f * x) / white;
}

// ── Reinhard (Reinhard et al., SIGGRAPH 2002) ─────────────────────────────────

/// Luminance-preserving Reinhard tone mapper.  Tone-maps luminance, preserves chromaticity.
[[nodiscard]] inline sm::float3 reinhardLuminance(sm::float3 x) noexcept {
    const float lum = ColorSpace::luminance(x);
    if (lum <= 0.f)
        return sm::float3{0.f, 0.f, 0.f};
    const float lumTm = lum / (1.f + lum);
    return x * (lumTm / lum);
}

} // namespace harmonia::utils::ToneMapping
#endif // HARMONIA_UTILS_TONEMAPPING_HPP
