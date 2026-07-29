#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <numbers>
#include <random>
#include <slang-math/slang-math.hpp>
#include <unordered_set>
#include <vector>

#include "harmonia/utils/Math.hpp"

namespace {
constexpr float kEpsilon = 1.0e-5F;
constexpr float kMonteCarloTolerance = 2.0e-2F;

[[nodiscard]] sm::float3 sampleUniformHemisphere(float u1, float u2) noexcept {
    const float z = u1;
    const float r = std::sqrt(std::max(0.0F, 1.0F - (z * z)));
    const float phi = Math::k2Pi * u2;
    return {r * std::cos(phi), r * std::sin(phi), z};
}

// TBN stored row-major: each row is one basis vector (t, b, n).
[[nodiscard]] sm::float3x3 buildTbn(sm::float3 n) noexcept {
    n = sm::normalize(n);
    const sm::float3 up = std::abs(n.z) < 0.999F ? sm::float3(0.0F, 0.0F, 1.0F) : sm::float3(0.0F, 1.0F, 0.0F);
    const sm::float3 t = sm::normalize(sm::cross(up, n));
    const sm::float3 b = sm::cross(n, t);
    return {t, b, n}; // rows: t=row0, b=row1, n=row2
}

[[nodiscard]] sm::float3 fresnelSchlick(sm::float3 f0, float cosTheta) noexcept {
    const float oneMinusCos = 1.0F - std::clamp(cosTheta, 0.0F, 1.0F);
    const float factor = oneMinusCos * oneMinusCos * oneMinusCos * oneMinusCos * oneMinusCos;
    return f0 + (sm::float3(1.0F, 1.0F, 1.0F) - f0) * factor;
}

[[nodiscard]] float ggxD(float nDotH, float alpha) noexcept {
    if (nDotH <= 0.0F)
        return 0.0F;
    const float alpha2 = alpha * alpha;
    const float denom = (nDotH * nDotH) * (alpha2 - 1.0F) + 1.0F;
    return alpha2 / (Math::kPi * denom * denom);
}

[[nodiscard]] float smithLambdaGgx(float nDotV, float alpha) noexcept {
    if (nDotV <= 0.0F)
        return std::numeric_limits<float>::infinity();
    const float cos2 = nDotV * nDotV;
    const float tan2 = (1.0F - cos2) / cos2;
    return 0.5F * (-1.0F + std::sqrt(1.0F + (alpha * alpha * tan2)));
}

[[nodiscard]] float smithG1(float nDotV, float alpha) noexcept {
    if (nDotV <= 0.0F)
        return 0.0F;
    return 1.0F / (1.0F + smithLambdaGgx(nDotV, alpha));
}

[[nodiscard]] float ggxG2(sm::float3 l, sm::float3 v, float alpha) noexcept {
    const float nDotL = std::max(l.z, 0.0F);
    const float nDotV = std::max(v.z, 0.0F);
    return smithG1(nDotL, alpha) * smithG1(nDotV, alpha);
}

[[nodiscard]] sm::float3 sampleGgxVndf(sm::float3 v, float alpha, sm::float2 u) noexcept {
    const sm::float3 vh = sm::normalize(sm::float3(alpha * v.x, alpha * v.y, v.z));
    const float lensq = (vh.x * vh.x) + (vh.y * vh.y);
    const sm::float3 t1 =
        lensq > 0.0F ? sm::float3(-vh.y, vh.x, 0.0F) / std::sqrt(lensq) : sm::float3(1.0F, 0.0F, 0.0F);
    const sm::float3 t2 = sm::cross(vh, t1);

    const float r = std::sqrt(u.x);
    const float phi = Math::k2Pi * u.y;
    float p1 = r * std::cos(phi);
    float p2 = r * std::sin(phi);
    const float s = 0.5F * (1.0F + vh.z);
    p2 = ((1.0F - s) * std::sqrt(std::max(0.0F, 1.0F - (p1 * p1)))) + (s * p2);

    const float p3 = std::sqrt(std::max(0.0F, 1.0F - (p1 * p1) - (p2 * p2)));
    const sm::float3 nh = (p1 * t1) + (p2 * t2) + (p3 * vh);
    return sm::normalize(sm::float3(alpha * nh.x, alpha * nh.y, std::max(0.0F, nh.z)));
}

[[nodiscard]] float ggxVndfPdf(sm::float3 v, sm::float3 m, float alpha) noexcept {
    const float nDotV = std::max(v.z, 0.0F);
    const float nDotM = std::max(m.z, 0.0F);
    const float vDotM = std::max(sm::dot(v, m), 0.0F);
    if (nDotV <= 0.0F || nDotM <= 0.0F || vDotM <= 0.0F)
        return 0.0F;
    return ggxD(nDotM, alpha) * smithG1(nDotV, alpha) * vDotM / nDotV;
}

[[nodiscard]] uint32_t wangHash(uint32_t seed) noexcept {
    seed = (seed ^ 61U) ^ (seed >> 16U);
    seed *= 9U;
    seed ^= seed >> 4U;
    seed *= 0x27d4eb2dU;
    seed ^= seed >> 15U;
    return seed;
}

[[nodiscard]] float randFloat(uint32_t& state) noexcept {
    state = wangHash(state);
    return static_cast<float>(state) * (1.0F / 4294967296.0F);
}
} // namespace

TEST(Math, BuildTbnIsOrthonormalAndStable) {
    const std::array normals{
        sm::normalize(sm::float3(0.0F, 1.0F, 0.0F)),
        sm::normalize(sm::float3(1.0F, 0.0F, 0.0F)),
        sm::normalize(sm::float3(0.0F, 0.0F, 1.0F)),
        sm::normalize(sm::float3(0.577F, 0.577F, 0.577F)),
    };

    for (const sm::float3 n : normals) {
        const sm::float3x3 tbn = buildTbn(n);
        const sm::float3 t = tbn[0]; // row 0 = tangent
        const sm::float3 b = tbn[1]; // row 1 = bitangent
        const sm::float3 z = tbn[2]; // row 2 = normal

        EXPECT_NEAR(sm::length(t), 1.0F, kEpsilon);
        EXPECT_NEAR(sm::length(b), 1.0F, kEpsilon);
        EXPECT_NEAR(sm::length(z), 1.0F, kEpsilon);
        EXPECT_NEAR(sm::dot(t, b), 0.0F, kEpsilon);
        EXPECT_NEAR(sm::dot(t, z), 0.0F, kEpsilon);
        EXPECT_NEAR(sm::dot(b, z), 0.0F, kEpsilon);
        EXPECT_NEAR(sm::determinant(tbn), 1.0F, 5.0e-4F);
        EXPECT_NEAR(sm::length(z - n), 0.0F, kEpsilon);
    }
}

TEST(Math, FresnelSchlickHasExpectedLimitsAndMonotonicity) {
    const sm::float3 f0(0.04F, 0.25F, 0.9F);

    const sm::float3 normalIncidence = fresnelSchlick(f0, 1.0F);
    const sm::float3 grazing = fresnelSchlick(f0, 0.0F);

    EXPECT_NEAR(sm::length(normalIncidence - f0), 0.0F, kEpsilon);
    EXPECT_NEAR(grazing.x, 1.0F, kEpsilon);
    EXPECT_NEAR(grazing.y, 1.0F, kEpsilon);
    EXPECT_NEAR(grazing.z, 1.0F, kEpsilon);

    sm::float3 previous = fresnelSchlick(f0, 1.0F);
    for (int i = 1; i <= 16; ++i) {
        const float cosTheta = 1.0F - (static_cast<float>(i) / 16.0F);
        const sm::float3 current = fresnelSchlick(f0, cosTheta);
        EXPECT_GE(current.x + kEpsilon, previous.x);
        EXPECT_GE(current.y + kEpsilon, previous.y);
        EXPECT_GE(current.z + kEpsilon, previous.z);
        EXPECT_LE(current.x, 1.0F + kEpsilon);
        EXPECT_LE(current.y, 1.0F + kEpsilon);
        EXPECT_LE(current.z, 1.0F + kEpsilon);
        previous = current;
    }
}

TEST(Math, GgxDIntegratesToOneOverHemisphere) {
    std::mt19937 rng(12345U);
    std::uniform_real_distribution<float> dist(0.0F, 1.0F);

    constexpr float alpha = 0.5F;
    constexpr int sampleCount = 50000;
    const float pdf = Math::kInv2Pi;

    double estimate = 0.0;
    for (int i = 0; i < sampleCount; ++i) {
        const sm::float3 h = sampleUniformHemisphere(dist(rng), dist(rng));
        estimate += ggxD(h.z, alpha) * h.z / pdf;
    }
    estimate /= static_cast<double>(sampleCount);

    EXPECT_NEAR(static_cast<float>(estimate), 1.0F, kMonteCarloTolerance);
}

TEST(Math, GgxG2StaysInRangeAndIsSymmetric) {
    constexpr float alpha = 0.5F;
    std::mt19937 rng(4242U);
    std::uniform_real_distribution<float> dist(0.0F, 1.0F);

    for (int i = 0; i < 1000; ++i) {
        const sm::float3 l = sampleUniformHemisphere(dist(rng), dist(rng));
        const sm::float3 v = sampleUniformHemisphere(dist(rng), dist(rng));
        const float gLv = ggxG2(l, v, alpha);
        const float gVl = ggxG2(v, l, alpha);
        EXPECT_GE(gLv, 0.0F);
        EXPECT_LE(gLv, 1.0F + kEpsilon);
        EXPECT_NEAR(gLv, gVl, 1.0e-6F);
    }

    EXPECT_NEAR(ggxG2(sm::float3(0.0F, 0.0F, 1.0F), sm::float3(0.0F, 0.0F, 1.0F), alpha), 1.0F, kEpsilon);
}

TEST(Math, SampleGgxVndfProducesNormalizedPdf) {
    constexpr float alpha = 0.5F;
    const sm::float3 v = sm::normalize(sm::float3(0.3F, -0.2F, 0.9327379F));

    std::mt19937 rng(7U);
    std::uniform_real_distribution<float> dist(0.0F, 1.0F);

    constexpr int integralSampleCount = 10000;
    const float hemispherePdf = Math::kInv2Pi;
    double pdfIntegral = 0.0;
    double pdfWeightedMeanCos = 0.0;
    for (int i = 0; i < integralSampleCount; ++i) {
        const sm::float3 m = sampleUniformHemisphere(dist(rng), dist(rng));
        const float pdf = ggxVndfPdf(v, m, alpha);
        pdfIntegral += pdf / hemispherePdf;
        pdfWeightedMeanCos += (m.z * pdf) / hemispherePdf;
    }
    pdfIntegral /= static_cast<double>(integralSampleCount);
    pdfWeightedMeanCos /= static_cast<double>(integralSampleCount);

    EXPECT_NEAR(static_cast<float>(pdfIntegral), 1.0F, 2.5e-2F);

    constexpr int sampleCount = 10000;
    double sampledMeanCos = 0.0;
    for (int i = 0; i < sampleCount; ++i) {
        const sm::float3 m = sampleGgxVndf(v, alpha, sm::float2(dist(rng), dist(rng)));
        EXPECT_NEAR(sm::length(m), 1.0F, 1.0e-4F);
        EXPECT_GE(m.z, -kEpsilon);
        sampledMeanCos += m.z;
    }
    sampledMeanCos /= static_cast<double>(sampleCount);

    EXPECT_NEAR(static_cast<float>(sampledMeanCos), static_cast<float>(pdfWeightedMeanCos), 3.0e-2F);
}

TEST(Math, WangHashSeparatesConsecutiveSeeds) {
    std::unordered_set<uint32_t> values;
    values.reserve(1000);

    for (uint32_t seed = 0; seed < 1000U; ++seed) {
        const uint32_t hash = wangHash(seed);
        EXPECT_TRUE(values.insert(hash).second) << "Duplicate hash for seed " << seed;
    }
}

TEST(Math, RandFloatStaysWithinUnitInterval) {
    uint32_t state = 1U;
    for (int i = 0; i < 10000; ++i) {
        const float value = randFloat(state);
        EXPECT_GE(value, 0.0F);
        EXPECT_LT(value, 1.0F);
    }
}
