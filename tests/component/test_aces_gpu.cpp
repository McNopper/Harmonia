#include <gtest/gtest.h>
#include <slang-math/slang-math.hpp>

#include "fixtures/ShaderDispatchFixture.hpp"

namespace {
void expectNear(sm::float4 actual, sm::float3 expected, float tolerance) {
    EXPECT_NEAR(actual.x, expected.x, tolerance);
    EXPECT_NEAR(actual.y, expected.y, tolerance);
    EXPECT_NEAR(actual.z, expected.z, tolerance);
}
} // namespace

TEST_F(ShaderDispatchFixture, AcesGpuMatchesOcioAndAcademyKnownAnswers) {
    const auto values = runShader("aces_test.spv", 5U);
    ASSERT_EQ(values.size(), 5U);
    expectNear(values[0], {0.453158317919F, 0.394926024520F, 0.299297344519F}, 2.0e-5F);
    expectNear(values[1], {0.7255029349F, 0.5745046219F, 0.4940730916F}, 5.0e-4F);
    expectNear(values[2], {0.3559542755F, 0.3559542755F, 0.3559542755F}, 5.0e-4F);
    expectNear(values[3], {0.8119707332F, 0.8119707332F, 0.8119707332F}, 5.0e-4F);
    expectNear(values[4], {0.34128153F, 0.32533440F, 0.24217427F}, 5.0e-4F);
}
