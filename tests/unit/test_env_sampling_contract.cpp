#include <algorithm>
#include <cmath>
#include <gtest/gtest.h>
#include <limits>
#include <slang-math/slang-math.hpp>
#include <vector>

#include "harmonia/utils/Rng.hpp"

namespace {

constexpr float kPi = 3.14159265359F;
[[maybe_unused]] constexpr float kInvPi = 0.31830988618F;

std::uint32_t sampleCdf1D(const std::vector<float>& cdf, std::uint32_t base, std::uint32_t n, float r) {
    std::uint32_t lo = 0U;
    std::uint32_t hi = n - 1U;
    while (lo < hi) {
        const std::uint32_t mid = (lo + hi) >> 1U;
        if (cdf[base + mid + 1U] <= r) {
            lo = mid + 1U;
        } else {
            hi = mid;
        }
    }
    return lo;
}

float sampleAndEvalPdf(std::uint32_t& rngState) {
    constexpr std::uint32_t W = 4U;
    constexpr std::uint32_t H = 2U;
    const std::vector<float> marginal{0.0F, 0.5F, 1.0F};
    const std::vector<float> conditional{
        0.0F,
        0.25F,
        0.5F,
        0.75F,
        1.0F,
        0.0F,
        0.25F,
        0.5F,
        0.75F,
        1.0F,
    };

    const sm::float2 xi = Rng::nextFloat2(rngState);
    const std::uint32_t row = sampleCdf1D(marginal, 0U, H, xi.x);
    const std::uint32_t col = sampleCdf1D(conditional, row * (W + 1U), W, xi.y);

    const float rowLo = marginal[row];
    const float rowHi = marginal[row + 1U];
    const float colLo = conditional[row * (W + 1U) + col];
    const float colHi = conditional[row * (W + 1U) + col + 1U];
    const float pRow = rowHi - rowLo;
    const float pCol = colHi - colLo;
    const float theta = kPi * (static_cast<float>(row) + 0.5F) / static_cast<float>(H);
    const float sinTheta = std::max(std::sin(theta), 1.0e-5F);
    return (pRow * pCol * static_cast<float>(W) * static_cast<float>(H)) / (2.0F * kPi * kPi * sinTheta);
}

float balanceHeuristic(float pA, float pB) {
    const float a = std::isfinite(pA) ? std::max(pA, 0.0F) : 0.0F;
    const float b = std::isfinite(pB) ? std::max(pB, 0.0F) : 0.0F;
    const float d = a + b;
    return d > 0.0F ? a / d : 0.0F;
}

} // namespace

TEST(EnvSamplingContract, DeterministicReplayKeepsPdfSequenceStable) {
    std::uint32_t s0 = Rng::composeSeed({11U, 13U}, 2U, 1U, 999U);
    std::uint32_t s1 = Rng::composeSeed({11U, 13U}, 2U, 1U, 999U);
    for (int i = 0; i < 16; ++i) {
        const float p0 = sampleAndEvalPdf(s0);
        const float p1 = sampleAndEvalPdf(s1);
        EXPECT_NEAR(p0, p1, 1.0e-6F);
        EXPECT_TRUE(std::isfinite(p0));
        EXPECT_GT(p0, 0.0F);
    }
}

TEST(EnvSamplingContract, BalanceHeuristicStaysBounded) {
    EXPECT_FLOAT_EQ(balanceHeuristic(0.0F, 0.0F), 0.0F);
    EXPECT_NEAR(balanceHeuristic(0.8F, 0.2F) + balanceHeuristic(0.2F, 0.8F), 1.0F, 1.0e-6F);
    EXPECT_GE(balanceHeuristic(0.8F, 0.2F), 0.0F);
    EXPECT_LE(balanceHeuristic(0.8F, 0.2F), 1.0F);
    EXPECT_FLOAT_EQ(balanceHeuristic(std::numeric_limits<float>::infinity(), 1.0F), 0.0F);
}
