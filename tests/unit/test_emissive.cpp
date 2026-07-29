#include <cmath>
#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

#include "harmonia/scene/EmissiveBuilder.hpp"

namespace {

constexpr float kEpsilon = 1.0e-6F;

// A valid selection CDF is non-decreasing, starts > 0, ends at exactly 1, in [0,1].
void expectValidCdf(const std::vector<float>& cdf) {
    ASSERT_FALSE(cdf.empty());
    EXPECT_GT(cdf.front(), 0.0F);
    EXPECT_NEAR(cdf.back(), 1.0F, kEpsilon);
    for (const float v : cdf) {
        EXPECT_GE(v, 0.0F);
        EXPECT_LE(v, 1.0F + kEpsilon);
    }
    for (size_t i = 1; i < cdf.size(); ++i) {
        EXPECT_GE(cdf[i] + kEpsilon, cdf[i - 1]) << "not non-decreasing at " << i;
    }
}

} // namespace

TEST(EmissiveCdf, EmptyReturnsSentinel) {
    const std::vector<float> power;
    const std::vector<float> cdf = harmonia::buildEmissiveCdf(power);
    ASSERT_EQ(cdf.size(), 1u);
    EXPECT_NEAR(cdf.back(), 1.0F, kEpsilon);
}

TEST(EmissiveCdf, SingleEmitterIsOne) {
    const std::vector<float> cdf = harmonia::buildEmissiveCdf({42.0F});
    ASSERT_EQ(cdf.size(), 1u);
    EXPECT_NEAR(cdf.front(), 1.0F, kEpsilon);
}

TEST(EmissiveCdf, EqualPowersAreUniform) {
    // Three equal-power emitters → a uniform selection CDF: [1/3, 2/3, 1].
    const std::vector<float> cdf = harmonia::buildEmissiveCdf({5.0F, 5.0F, 5.0F});
    ASSERT_EQ(cdf.size(), 3u);
    expectValidCdf(cdf);
    EXPECT_NEAR(cdf[0], 1.0F / 3.0F, kEpsilon);
    EXPECT_NEAR(cdf[1], 2.0F / 3.0F, kEpsilon);
}

TEST(EmissiveCdf, ProportionalToPower) {
    // powers [1, 1, 2] → total 4 → cdf [0.25, 0.5, 1.0].
    const std::vector<float> cdf = harmonia::buildEmissiveCdf({1.0F, 1.0F, 2.0F});
    ASSERT_EQ(cdf.size(), 3u);
    expectValidCdf(cdf);
    EXPECT_NEAR(cdf[0], 0.25F, kEpsilon);
    EXPECT_NEAR(cdf[1], 0.50F, kEpsilon);
    EXPECT_NEAR(cdf[2], 1.00F, kEpsilon);
}

TEST(EmissiveCdf, AllZeroPowersFallBackToUniform) {
    // Degenerate/black emitters (all zero power) → uniform CDF so sampling stays valid.
    const std::vector<float> cdf = harmonia::buildEmissiveCdf({0.0F, 0.0F, 0.0F, 0.0F});
    ASSERT_EQ(cdf.size(), 4u);
    expectValidCdf(cdf);
    for (uint32_t i = 0; i < 4u; ++i) {
        EXPECT_NEAR(cdf[i], static_cast<float>(i + 1) / 4.0F, kEpsilon);
    }
}

TEST(EmissiveCdf, RoundingGuardForcesLastToOne) {
    // Many tiny equal powers can accumulate double-precision drift; the builder pins cdf[N-1]=1.
    std::vector<float> power(1000, 1.0e-3F);
    const std::vector<float> cdf = harmonia::buildEmissiveCdf(power);
    ASSERT_EQ(cdf.size(), 1000u);
    expectValidCdf(cdf);
}

// Inverse-CDF sample: a power-proportional CDF must select emitter i with probability
// power_i / totalPower. Verify empirically by sampling the CDF with a uniform variate.
TEST(EmissiveCdf, SamplingMatchesPowerDistribution) {
    const std::vector<float> power{1.0F, 3.0F, 2.0F}; // selection probs {0.1667, 0.5, 0.3333}
    const std::vector<float> cdf = harmonia::buildEmissiveCdf(power);
    const float total = 6.0F;

    const uint32_t steps = 10000u;
    std::vector<uint32_t> hits(cdf.size(), 0u);
    for (uint32_t s = 0; s < steps; ++s) {
        const float u = (static_cast<float>(s) + 0.5F) / static_cast<float>(steps); // stratified in (0,1)
        uint32_t idx = 0u;
        while (idx + 1u < cdf.size() && u > cdf[idx]) {
            ++idx;
        }
        ++hits[idx];
    }
    for (size_t i = 0; i < cdf.size(); ++i) {
        const float expected = power[i] / total;
        const float actual = static_cast<float>(hits[i]) / static_cast<float>(steps);
        EXPECT_NEAR(actual, expected, 1.0e-3F) << "emitter " << i;
    }
}
