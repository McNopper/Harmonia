#pragma once

#include <slang-math/slang-math.hpp>

#include <cmath>
#include <limits>
#include <numbers>

namespace Math {
inline constexpr float kPi = std::numbers::pi_v<float>;
inline constexpr float k2Pi = 2.0F * kPi;
inline constexpr float kInvPi = 1.0F / kPi;
inline constexpr float kInv2Pi = 1.0F / k2Pi;

[[nodiscard]] inline sm::float4x4 makeRotationY(float radians) noexcept {
    return sm::rotate(sm::float4x4(1.0f), radians, sm::float3(0.0f, 1.0f, 0.0f));
}

[[nodiscard]] inline sm::float3 safeDivide(sm::float3 a, float b) noexcept {
    constexpr float kEpsilon = 1.0e-8F;
    if (std::abs(b) <= kEpsilon) {
        return sm::float3{0.0F, 0.0F, 0.0F};
    }
    return a / b;
}

[[nodiscard]] inline float luminance(sm::float3 c) noexcept {
    return (0.2627F * c.r) + (0.6780F * c.g) + (0.0593F * c.b);
}

[[nodiscard]] inline sm::float3 srgbToLinear(sm::float3 c) noexcept {
    const sm::float3 clamped = sm::max(c, sm::float3{0.0F, 0.0F, 0.0F});
    return sm::float3{std::pow(clamped.x, 2.2F), std::pow(clamped.y, 2.2F), std::pow(clamped.z, 2.2F)};
}

[[nodiscard]] inline bool isNanOrInf(sm::float3 v) noexcept {
    return std::isnan(v.x) || std::isnan(v.y) || std::isnan(v.z) || std::isinf(v.x) || std::isinf(v.y) ||
           std::isinf(v.z);
}
} // namespace Math
