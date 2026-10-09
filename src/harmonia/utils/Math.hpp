#ifndef HARMONIA_UTILS_MATH_HPP
#define HARMONIA_UTILS_MATH_HPP

#include <cmath>
#include <limits>
#include <numbers>
#include <slang-math/slang-math.hpp>

#include "harmonia/utils/ColorSpace.hpp"

namespace harmonia::Math {
inline constexpr float kPi = std::numbers::pi_v<float>;
inline constexpr float k2Pi = 2.0F * kPi;
inline constexpr float kInvPi = 1.0F / kPi;
inline constexpr float kInv2Pi = 1.0F / k2Pi;
} // namespace harmonia::Math
#endif // HARMONIA_UTILS_MATH_HPP
