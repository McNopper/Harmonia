#pragma once

// Hyperion ColorSpace utility — the working color space is linear Rec.2020 by
// default; scenes may select linear Rec.709 instead (WorkingColorSpace below).
// Conversions to/from other spaces are provided only for I/O boundaries.
//
// Reference primaries (ITU-R BT.2020):
//   R: (0.708, 0.292)  G: (0.170, 0.797)  B: (0.131, 0.046)  D65 white
//
// Luminance coefficients (BT.2020):  Kr=0.2627  Kg=0.6780  Kb=0.0593

#include <slang-math/slang-math.hpp>

#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ColorSpace {

// ── Working color space ──────────────────────────────────────────────────────
// The scene-referred space all rendering happens in — always linear; only the
// primaries differ. Selected per scene ([render] working_color_space, token =
// ColorInterop interop ID). Assets are converted into it at load time; the
// ToneMapper converts from it to the display-referred output space.
// Values mirror tonemap.slang's workingColorSpace push constant.
enum class WorkingColorSpace : uint32_t {
    LinRec2020 = 0, ///< "lin_rec2020_scene" — default
    LinRec709 = 1,  ///< "lin_rec709_scene"
};

/// Parse a ColorInterop interop ID into a working color space.
/// Returns nullopt for unsupported tokens.
[[nodiscard]] inline std::optional<WorkingColorSpace> parseWorkingColorSpace(std::string_view token) noexcept {
    if (token == "lin_rec2020_scene")
        return WorkingColorSpace::LinRec2020;
    if (token == "lin_rec709_scene")
        return WorkingColorSpace::LinRec709;
    return std::nullopt;
}

/// ColorInterop interop ID of a working color space (for logs / metadata).
[[nodiscard]] inline const char* interopId(WorkingColorSpace ws) noexcept {
    return ws == WorkingColorSpace::LinRec709 ? "lin_rec709_scene" : "lin_rec2020_scene";
}

// ── Luminance (Rec.2020 relative, scene-linear) ─────────────────────────────
// Returns relative luminance Y in Rec.2020; input must be linear Rec.2020.
[[nodiscard]] inline float luminance(sm::float3 linearRec2020) noexcept {
    return 0.2627f * linearRec2020.r + 0.6780f * linearRec2020.g + 0.0593f * linearRec2020.b;
}

// ── Rec.2020 ↔ Rec.709 (linear) ─────────────────────────────────────────────
// Use for display output only — never convert mid-pipeline.
[[nodiscard]] sm::float3 rec2020ToRec709(sm::float3 c) noexcept;
[[nodiscard]] sm::float3 rec709ToRec2020(sm::float3 c) noexcept;

// ── Rec.2020 ↔ CIE XYZ D65 ──────────────────────────────────────────────────
[[nodiscard]] sm::float3 rec2020ToXyz(sm::float3 c) noexcept;
[[nodiscard]] sm::float3 xyzToRec2020(sm::float3 xyz) noexcept;

// ── Rec.2020 ↔ ACES AP1 (ACEScg) ────────────────────────────────────────────
[[nodiscard]] sm::float3 rec2020ToAcesCg(sm::float3 c) noexcept;
[[nodiscard]] sm::float3 acesCgToRec2020(sm::float3 c) noexcept;

// ── sRGB / Rec.709 OETF & EOTF ──────────────────────────────────────────────
// Used at SDR display output: linearize asset textures on upload,
// or encode to sRGB at display boundary.
// Input/output: linear Rec.709 (NOT Rec.2020 — convert primaries first).
[[nodiscard]] sm::float3 linearRec709ToSrgb(sm::float3 linear) noexcept;
[[nodiscard]] sm::float3 srgbToLinearRec709(sm::float3 srgb) noexcept;

// Convenience: sRGB asset texture → linear Rec.2020 (two-step, used on upload)
[[nodiscard]] inline sm::float3 srgbAssetToRec2020(sm::float3 srgb) noexcept {
    return rec709ToRec2020(srgbToLinearRec709(srgb));
}

// ── Peak luminance constants ─────────────────────────────────────────────────
inline constexpr float kPeakLuminanceHDR10Nits = 10000.0f; // ST.2084 reference
inline constexpr float kPeakLuminanceSDRNits = 100.0f;     // sRGB / Rec.709

// ── PQ (ST.2084) OETF & EOTF ────────────────────────────────────────────────
// HDR10 display output. Input to pqOetf: absolute luminance in nits / 10000.
// i.e. normalise: Yn = clamp(nits / 10000, 0, 1) before calling.
[[nodiscard]] float pqOetf(float Yn) noexcept;
[[nodiscard]] sm::float3 pqOetf(sm::float3 Yn) noexcept;
[[nodiscard]] float pqEotf(float E) noexcept; // inverse — display→scene
[[nodiscard]] sm::float3 pqEotf(sm::float3 E) noexcept;

/// Convenience wrapper: absolute nits → PQ code value.
/// Equivalent to pqOetf(nits / kPeakLuminanceHDR10Nits).
[[nodiscard]] inline float pqOetfFromNits(float nits) noexcept {
    return pqOetf(nits / kPeakLuminanceHDR10Nits);
}
[[nodiscard]] inline sm::float3 pqOetfFromNits(sm::float3 nits) noexcept {
    return pqOetf(nits / kPeakLuminanceHDR10Nits);
}

// ── HLG (BT.2100) OETF ───────────────────────────────────────────────────────
// HLG display output. Input: scene-linear, 1.0 = HLG reference white.
// Constants per ITU-R BT.2100 Table 5: a = 0.17883277, b = 1-4a, c = 0.5-a*ln(4a).
[[nodiscard]] float hlgOetf(float E) noexcept;
[[nodiscard]] sm::float3 hlgOetf(sm::float3 E) noexcept;

// ── Exposure ─────────────────────────────────────────────────────────────────
// EV100 → linear exposure multiplier (UE5 / photographic convention).
// Apply to linear Rec.2020 scene radiance before tone mapping.
[[nodiscard]] inline float ev100ToExposure(float ev100) noexcept {
    // exposure = 1 / (1.2 * 2^EV100)
    return 1.0f / (1.2f * std::exp2(ev100));
}

} // namespace ColorSpace
