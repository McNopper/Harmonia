#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace Rng {

[[nodiscard]] inline uint32_t wangHash(uint32_t seed) noexcept {
    seed = (seed ^ 61U) ^ (seed >> 16U);
    seed *= 9U;
    seed ^= (seed >> 4U);
    seed *= 0x27d4eb2dU;
    seed ^= (seed >> 15U);
    return seed;
}

[[nodiscard]] inline uint32_t hashCombine(uint32_t seed, uint32_t value) noexcept {
    return wangHash(seed ^ (value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U)));
}

[[nodiscard]] inline uint32_t composeSeed(glm::uvec2 pixel,
                                          uint32_t frameSampleIndex,
                                          uint32_t bounceIndex,
                                          uint32_t baseSeed) noexcept {
    uint32_t seed = wangHash(baseSeed);
    seed = hashCombine(seed, pixel.x);
    seed = hashCombine(seed, pixel.y);
    seed = hashCombine(seed, frameSampleIndex);
    seed = hashCombine(seed, bounceIndex);
    return seed;
}

[[nodiscard]] inline float nextFloat(uint32_t& state) noexcept {
    state = wangHash(state);
    return static_cast<float>(state & 0x00ffffffU) * (1.0F / 16777216.0F);
}

[[nodiscard]] inline glm::vec2 nextFloat2(uint32_t& state) noexcept {
    return glm::vec2(nextFloat(state), nextFloat(state));
}

} // namespace Rng
