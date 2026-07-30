#ifndef HARMONIA_UTILS_RNG_HPP
#define HARMONIA_UTILS_RNG_HPP

#include <cstdint>
#include <slang-math/slang-math.hpp>

namespace harmonia::Rng {

[[nodiscard]] inline std::uint32_t wangHash(std::uint32_t seed) noexcept {
    seed = (seed ^ 61U) ^ (seed >> 16U);
    seed *= 9U;
    seed ^= (seed >> 4U);
    seed *= 0x27d4eb2dU;
    seed ^= (seed >> 15U);
    return seed;
}

[[nodiscard]] inline std::uint32_t hashCombine(std::uint32_t seed, std::uint32_t value) noexcept {
    return wangHash(seed ^ (value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U)));
}

[[nodiscard]] inline std::uint32_t composeSeed(sm::uint2 pixel,
                                               std::uint32_t frameSampleIndex,
                                               std::uint32_t bounceIndex,
                                               std::uint32_t baseSeed) noexcept {
    std::uint32_t seed = wangHash(baseSeed);
    seed = hashCombine(seed, pixel.x);
    seed = hashCombine(seed, pixel.y);
    seed = hashCombine(seed, frameSampleIndex);
    seed = hashCombine(seed, bounceIndex);
    return seed;
}

[[nodiscard]] inline float nextFloat(std::uint32_t& state) noexcept {
    state = wangHash(state);
    return static_cast<float>(state & 0x00ffffffU) * (1.0F / 16777216.0F);
}

[[nodiscard]] inline sm::float2 nextFloat2(std::uint32_t& state) noexcept {
    return sm::float2{nextFloat(state), nextFloat(state)};
}

} // namespace harmonia::Rng
#endif // HARMONIA_UTILS_RNG_HPP
