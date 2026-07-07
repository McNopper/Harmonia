
#include <slang-math/slang-math.hpp>

#include <array>
#include <gtest/gtest.h>

#include "harmonia/utils/ColorSpace.hpp"

namespace {
constexpr float kEpsilon = 1.0e-5F;
}

TEST(ColorSpace, SrgbRoundTripsWithinTolerance) {
    const std::array values{
        sm::float3(0.0F, 0.0F, 0.0F),
        sm::float3(0.18F, 0.18F, 0.18F),
        sm::float3(1.0F, 1.0F, 1.0F),
        sm::float3(0.25F, 0.5F, 0.75F),
        sm::float3(0.0031308F, 0.04045F, 0.9F),
    };

    for (const sm::float3 srgb : values) {
        const sm::float3 linear = ColorSpace::srgbToLinearRec709(srgb);
        const sm::float3 roundtrip = ColorSpace::linearRec709ToSrgb(linear);
        EXPECT_NEAR(roundtrip.x, srgb.x, kEpsilon);
        EXPECT_NEAR(roundtrip.y, srgb.y, kEpsilon);
        EXPECT_NEAR(roundtrip.z, srgb.z, kEpsilon);
    }
}

TEST(ColorSpace, PqOetfHasExpectedEndpointsAndMonotonicity) {
    EXPECT_NEAR(ColorSpace::pqOetf(0.0F), 0.0F, kEpsilon);
    EXPECT_NEAR(ColorSpace::pqOetf(1.0F), 1.0F, 1.0e-4F);

    const std::array samples{0.0F, 1.0e-4F, 0.01F, 0.1F, 0.5F, 1.0F};
    float previous = -1.0F;
    for (const float sample : samples) {
        const float encoded = ColorSpace::pqOetf(sample);
        EXPECT_GE(encoded, previous);
        previous = encoded;
    }
}

TEST(ColorSpace, PqOetfVectorOverloadMatchesScalar) {
    const sm::float3 linear(0.0F, 0.18F, 1.0F);
    const sm::float3 encoded = ColorSpace::pqOetf(linear);

    EXPECT_NEAR(encoded.x, ColorSpace::pqOetf(linear.x), kEpsilon);
    EXPECT_NEAR(encoded.y, ColorSpace::pqOetf(linear.y), kEpsilon);
    EXPECT_NEAR(encoded.z, ColorSpace::pqOetf(linear.z), kEpsilon);
}

TEST(ColorSpace, Rec2020WhitePointRemainsNeutral) {
    const sm::float3 white = ColorSpace::rec709ToRec2020(sm::float3(1.0F, 1.0F, 1.0F));
    EXPECT_NEAR(white.x, 1.0F, kEpsilon);
    EXPECT_NEAR(white.y, 1.0F, kEpsilon);
    EXPECT_NEAR(white.z, 1.0F, kEpsilon);
}

TEST(ColorSpace, Rec2020ConversionPreservesBlack) {
    const sm::float3 black = ColorSpace::rec709ToRec2020(sm::float3(0.0F, 0.0F, 0.0F));
    EXPECT_NEAR(sm::length(black), 0.0F, kEpsilon);
}
