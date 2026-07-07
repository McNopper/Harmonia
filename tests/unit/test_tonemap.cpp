#include <slang-math/slang-math.hpp>

#include <array>
#include <cmath>
#include <gtest/gtest.h>

#include "harmonia/utils/ColorSpace.hpp"
#include "harmonia/utils/ToneMapping.hpp"

namespace {
constexpr float kEps = 1.0e-4F;
constexpr float kEpsHi = 1.0e-3F;
} // namespace

TEST(PqOetf, ReferenceNitValues) {
    struct Case { float nits; float expected; };
    const std::array<Case, 5> cases{{
        {1.0f, 0.149946f},
        {100.0f, 0.508078f},
        {203.0f, 0.580689f},
        {1000.0f, 0.751827f},
        {10000.0f, 1.000000f},
    }};
    for (const auto& c : cases) {
        EXPECT_NEAR(ColorSpace::pqOetfFromNits(c.nits), c.expected, kEps) << "nits = " << c.nits;
    }
}

TEST(PqOetf, BlackMapsToNearZero) {
    EXPECT_NEAR(ColorSpace::pqOetfFromNits(0.0f), 0.0f, 1.0e-4f);
}

TEST(PqOetf, IsMonotonicallyIncreasing) {
    constexpr std::array<float, 6> nitsSteps{0.f, 1.f, 100.f, 203.f, 1000.f, 10000.f};
    float prev = -1.f;
    for (float n : nitsSteps) {
        const float e = ColorSpace::pqOetfFromNits(n);
        EXPECT_GE(e, prev);
        prev = e;
    }
}

TEST(HlgOetf, ZeroMapsToZero) {
    EXPECT_NEAR(ColorSpace::hlgOetf(0.0f), 0.0f, 1.0e-6f);
}

TEST(HlgOetf, KneePointIsExactHalf) {
    const float knee = 1.0f / 12.0f;
    EXPECT_NEAR(ColorSpace::hlgOetf(knee), 0.5f, kEps);
}

TEST(HlgOetf, OneMapsToPeakNearOne) {
    EXPECT_NEAR(ColorSpace::hlgOetf(1.0f), 1.0f, kEps);
}

TEST(HlgOetf, IsMonotonicallyIncreasing) {
    const std::array<float, 5> steps{0.f, 1.f / 12.f, 0.25f, 0.5f, 1.0f};
    float prev = -1.f;
    for (float e : steps) {
        const float encoded = ColorSpace::hlgOetf(e);
        EXPECT_GE(encoded, prev);
        prev = encoded;
    }
}

TEST(HlgOetf, VectorOverloadMatchesScalar) {
    const sm::float3 v(0.0f, 1.0f / 12.0f, 1.0f);
    const sm::float3 enc = ColorSpace::hlgOetf(v);
    EXPECT_NEAR(enc.x, ColorSpace::hlgOetf(v.x), 1.0e-6f);
    EXPECT_NEAR(enc.y, ColorSpace::hlgOetf(v.y), 1.0e-6f);
    EXPECT_NEAR(enc.z, ColorSpace::hlgOetf(v.z), 1.0e-6f);
}

TEST(AcesToneMap, WhiteNeutralInLinearAP1) {
    const sm::float3 grey(1.0f, 1.0f, 1.0f);
    const sm::float3 result = ToneMapping::acesRrtOdtFit(grey);
    EXPECT_NEAR(result.x, result.y, kEps);
    EXPECT_NEAR(result.y, result.z, kEps);
}

TEST(AcesToneMap, OutputIsBoundedAfterFit) {
    const std::array<float, 5> inputs{0.f, 0.18f, 1.f, 4.f, 8.f};
    for (float v : inputs) {
        const sm::float3 out = ToneMapping::acesFittedSDR(sm::float3(v, v, v));
        EXPECT_GE(out.x, 0.f) << "v = " << v;
        EXPECT_LE(out.x, 1.1f) << "v = " << v;
    }
}

TEST(AcesToneMap, IsMonotonicallyIncreasingOnGrey) {
    float prev = -1.f;
    for (float v : {0.f, 0.01f, 0.1f, 0.18f, 0.5f, 1.f, 4.f, 10.f}) {
        const float out = ToneMapping::acesRrtOdtFit(sm::float3(v, v, v)).x;
        EXPECT_GE(out, prev) << "v = " << v;
        prev = out;
    }
}

TEST(AcesToneMap, DarkShadowsPreservedRelative) {
    const sm::float3 dark(0.001f, 0.001f, 0.001f);
    const sm::float3 out = ToneMapping::acesFittedSDR(dark);
    EXPECT_GE(out.x, 0.f);
    EXPECT_LT(out.x, 0.05f);
}

TEST(AcesToneMap, SDRFittedOutputInGamutRange) {
    const std::array<sm::float3, 5> hdr2020{{
        {0.f, 0.f, 0.f},
        {0.18f, 0.18f, 0.18f},
        {1.f, 1.f, 1.f},
        {4.f, 2.f, 0.5f},
        {0.f, 0.f, 8.f},
    }};
    for (const auto& c : hdr2020) {
        const sm::float3 out = ToneMapping::acesFittedSDR(c);
        EXPECT_FALSE(std::isnan(out.x));
        EXPECT_FALSE(std::isnan(out.y));
        EXPECT_FALSE(std::isnan(out.z));
        const sm::float3 clamped = sm::clamp(out, 0.f, 1.f);
        EXPECT_GE(clamped.x, 0.f);
        EXPECT_LE(clamped.x, 1.f);
    }
}

TEST(HableToneMap, WhiteNeutralOnGrey) {
    const sm::float3 grey(1.f, 1.f, 1.f);
    const sm::float3 out = ToneMapping::hableFilmic(grey);
    EXPECT_NEAR(out.x, out.y, kEps);
    EXPECT_NEAR(out.y, out.z, kEps);
}

TEST(HableToneMap, OutputBelowOne) {
    for (float v : {0.18f, 1.f, 4.f, 5.f}) {
        const sm::float3 out = ToneMapping::hableFilmic(sm::float3(v, v, v));
        EXPECT_LE(out.x, 1.0f + kEps) << "v = " << v;
        EXPECT_GE(out.x, 0.f);
    }
}

TEST(ReinhardToneMap, BlackStaysBlack) {
    const sm::float3 out = ToneMapping::reinhardLuminance(sm::float3(0.f, 0.f, 0.f));
    EXPECT_NEAR(sm::length(out), 0.f, 1.0e-6f);
}

TEST(ReinhardToneMap, OutputStrictlyBelowOne) {
    for (float v : {1.f, 4.f, 100.f}) {
        const sm::float3 out = ToneMapping::reinhardLuminance(sm::float3(v, v, v));
        EXPECT_LT(out.x, 1.0f);
        EXPECT_GE(out.x, 0.f);
    }
}

TEST(AcesMatrices, Rec2020WhitePointPreservedInAP1) {
    const sm::float3 white2020(1.f, 1.f, 1.f);
    const sm::float3 ap1 = ToneMapping::kRec2020ToAP1 * white2020;
    EXPECT_NEAR(ap1.x, 1.f, kEpsHi);
    EXPECT_NEAR(ap1.y, 1.f, kEpsHi);
    EXPECT_NEAR(ap1.z, 1.f, kEpsHi);
}

TEST(AcesMatrices, AP1WhitePointPreservedInRec709) {
    const sm::float3 whiteAP1(1.f, 1.f, 1.f);
    const sm::float3 rec709 = ToneMapping::kAP1ToRec709 * whiteAP1;
    EXPECT_NEAR(rec709.x, 1.f, kEpsHi);
    EXPECT_NEAR(rec709.y, 1.f, kEpsHi);
    EXPECT_NEAR(rec709.z, 1.f, kEpsHi);
}

TEST(AcesMatrices, AP1WhitePointPreservedInP3) {
    const sm::float3 whiteAP1(1.f, 1.f, 1.f);
    const sm::float3 p3 = ToneMapping::kAP1ToP3 * whiteAP1;
    EXPECT_NEAR(p3.x, 1.f, kEpsHi);
    EXPECT_NEAR(p3.y, 1.f, kEpsHi);
    EXPECT_NEAR(p3.z, 1.f, kEpsHi);
}
