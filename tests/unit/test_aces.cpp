#include <gtest/gtest.h>
#include <slang-math/slang-math.hpp>

#include "harmonia/utils/Aces.hpp"

namespace {
void expectNear(sm::float3 actual, sm::float3 expected, float tolerance) {
    EXPECT_NEAR(actual.x, expected.x, tolerance);
    EXPECT_NEAR(actual.y, expected.y, tolerance);
    EXPECT_NEAR(actual.z, expected.z, tolerance);
}
} // namespace

TEST(Aces, AcesCgToAces2065MatchesOcioVector) {
    // OCIO BuiltinTransform_tests: ACEScg_to_ACES2065-1, lossless processing.
    expectNear(harmonia::utils::Aces::acesCgToAces2065({0.5F, 0.4F, 0.3F}),
               {0.453158317919F, 0.394926024520F, 0.299297344519F},
               2.0e-6F);
}

TEST(Aces, SegmentedSplineAnchorsMatchAcademyCtl) {
    EXPECT_NEAR(harmonia::utils::Aces::segmentedSplineC5(0.0F), 0.0001F, 1.0e-7F);
    EXPECT_NEAR(harmonia::utils::Aces::segmentedSplineC5(0.18F), 4.8F, 2.0e-5F);
    EXPECT_NEAR(harmonia::utils::Aces::segmentedSplineC5(1.0F), 54.0134517808F, 2.0e-3F);
    EXPECT_NEAR(harmonia::utils::Aces::segmentedSplineC5(0.18F * 262144.0F), 10000.0F, 0.1F);
}

TEST(Aces, RrtMatchesIndependentOcioChainVector) {
    expectNear(harmonia::utils::Aces::rrt10AcesToOces({0.18F, 0.18F, 0.18F}), {4.8F, 4.8F, 4.8F}, 2.0e-4F);
    expectNear(harmonia::utils::Aces::rrt10AcesToOces({1.0F, 1.0F, 1.0F}),
               {54.0134517808F, 54.0134517808F, 54.0134517808F},
               2.0e-3F);
    expectNear(harmonia::utils::Aces::rrt10AcesToOces({0.5F, 0.4F, 0.3F}),
               {20.8525186750F, 15.8406257699F, 10.6484005881F},
               2.0e-3F);
}

TEST(Aces, FullSdrVideoMatchesOcioXyzAndSrgbVectors) {
    const sm::float3 ap0{0.5F, 0.4F, 0.3F};
    const auto oces = harmonia::utils::Aces::rrt10AcesToOces(ap0);
    expectNear(harmonia::utils::Aces::odt10SdrVideoToXyzD65(oces), {0.34128153F, 0.32533440F, 0.24217427F}, 2.0e-4F);
    expectNear(
        harmonia::utils::Aces::aces10SdrVideoToSrgb(ap0), {0.7255029349F, 0.5745046219F, 0.4940730916F}, 2.0e-4F);
}

TEST(Aces, NeutralSdrAnchorsAreNotExposureNormalized) {
    expectNear(harmonia::utils::Aces::aces10SdrVideoToSrgb({0.0F, 0.0F, 0.0F}), {0.0F, 0.0F, 0.0F}, 1.0e-6F);
    expectNear(harmonia::utils::Aces::aces10SdrVideoToSrgb({0.18F, 0.18F, 0.18F}),
               {0.3559542755F, 0.3559542755F, 0.3559542755F},
               2.0e-4F);
    expectNear(harmonia::utils::Aces::aces10SdrVideoToSrgb({1.0F, 1.0F, 1.0F}),
               {0.8119707332F, 0.8119707332F, 0.8119707332F},
               2.0e-4F);
    const float white = 0.18F * 90.5096679919F; // 2^6.5
    expectNear(harmonia::utils::Aces::aces10SdrVideoToSrgb({white, white, white}), {1.0F, 1.0F, 1.0F}, 2.0e-4F);
}
