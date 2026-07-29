#include <cstdint>
#include <gtest/gtest.h>
#include <unordered_set>

#include "harmonia/utils/Rng.hpp"

TEST(Rng, ComposeSeedIsDeterministicForSameInputs) {
    const std::uint32_t a = Rng::composeSeed({17U, 42U}, 9U, 3U, 1234U);
    const std::uint32_t b = Rng::composeSeed({17U, 42U}, 9U, 3U, 1234U);
    EXPECT_EQ(a, b);
}

TEST(Rng, ComposeSeedSeparatesPixelFrameAndBounceAxes) {
    const std::uint32_t base = Rng::composeSeed({8U, 9U}, 4U, 2U, 99U);
    EXPECT_NE(base, Rng::composeSeed({9U, 9U}, 4U, 2U, 99U));
    EXPECT_NE(base, Rng::composeSeed({8U, 10U}, 4U, 2U, 99U));
    EXPECT_NE(base, Rng::composeSeed({8U, 9U}, 5U, 2U, 99U));
    EXPECT_NE(base, Rng::composeSeed({8U, 9U}, 4U, 3U, 99U));
}

TEST(Rng, NextFloatStaysInUnitInterval) {
    std::uint32_t state = Rng::composeSeed({3U, 7U}, 1U, 0U, 5U);
    for (int i = 0; i < 10000; ++i) {
        const float value = Rng::nextFloat(state);
        EXPECT_GE(value, 0.0F);
        EXPECT_LT(value, 1.0F);
    }
}

TEST(Rng, FirstSamplesDecorrelateAcrossPixelsAndFrames) {
    std::unordered_set<std::uint32_t> fingerprints;
    fingerprints.reserve(64);

    for (std::uint32_t y = 0; y < 4U; ++y) {
        for (std::uint32_t x = 0; x < 4U; ++x) {
            for (std::uint32_t frame = 0; frame < 4U; ++frame) {
                std::uint32_t state = Rng::composeSeed({x, y}, frame, 0U, 0x12345678U);
                const std::uint32_t fingerprint = static_cast<std::uint32_t>(Rng::nextFloat(state) * 16777216.0F);
                EXPECT_TRUE(fingerprints.insert(fingerprint).second);
            }
        }
    }
}
