#ifndef HARMONIA_UTILS_ACES_HPP
#define HARMONIA_UTILS_ACES_HPP

#include <slang-math/slang-math.hpp>

namespace harmonia::utils::Aces {

// ACES 1.3 reference configuration, ACES 1.0 SDR Video rendering:
// RRT.a1.0.3 + ODT.Academy.RGBmonitor_100nits_dim.a1.0.3.
// These are the full Academy transforms, not the fitted ACES approximation.

[[nodiscard]] float segmentedSplineC5(float x) noexcept;
[[nodiscard]] float segmentedSplineC9Odt48(float x) noexcept;
[[nodiscard]] sm::float3 acesCgToAces2065(sm::float3 ap1) noexcept;
[[nodiscard]] sm::float3 aces2065ToAcesCg(sm::float3 ap0) noexcept;
[[nodiscard]] sm::float3 rrt10AcesToOces(sm::float3 ap0) noexcept;
[[nodiscard]] sm::float3 odt10SdrVideoToXyzD65(sm::float3 ocesAp0) noexcept;
[[nodiscard]] sm::float3 odt10SdrVideoToLinearRec709(sm::float3 ocesAp0) noexcept;
[[nodiscard]] sm::float3 odt10SdrVideoToSrgb(sm::float3 ocesAp0) noexcept;
[[nodiscard]] sm::float3 aces10SdrVideoToSrgb(sm::float3 ap0) noexcept;
[[nodiscard]] sm::float3 aces10SdrVideoToSrgbFromAcesCg(sm::float3 ap1) noexcept;

} // namespace harmonia::utils::Aces

#endif // HARMONIA_UTILS_ACES_HPP
