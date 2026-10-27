#ifndef HARMONIA_UTILS_MATH_HPP
#define HARMONIA_UTILS_MATH_HPP

#include <cmath>
#include <limits>
#include <slang-math/slang-math.hpp>

#include "harmonia/utils/ColorSpace.hpp"

namespace harmonia::utils::Math {
// SM6: slang-math is the single source of π — the local std::numbers mirror is gone.
inline constexpr float kPi = sm::pi<float>();
inline constexpr float k2Pi = 2.0F * kPi;
inline constexpr float kInvPi = 1.0F / kPi;
inline constexpr float kInv2Pi = 1.0F / k2Pi;
} // namespace harmonia::utils::Math
#endif // HARMONIA_UTILS_MATH_HPP
