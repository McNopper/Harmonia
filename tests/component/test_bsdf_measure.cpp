#include <gtest/gtest.h>
#include <slang-math/slang-math.hpp>

#include "fixtures/ShaderDispatchFixture.hpp"

TEST_F(ShaderDispatchFixture, BsdfMeasure_DiscreteEventsHaveNoContinuousDensity) {
    const auto values = runShader("bsdf_measure_test.spv", 12U);
    ASSERT_EQ(values.size(), 12U);
    for (auto i : {0U, 2U, 3U, 6U, 10U, 11U}) {
        SCOPED_TRACE(i);
        EXPECT_EQ(values[i].x, 0.0F);
        EXPECT_EQ(values[i].y, 0.0F);
        EXPECT_EQ(values[i].z, 1.0F);
        EXPECT_GT(values[i].w, 0.0F);
    }
    for (auto i : {1U, 4U, 5U, 7U, 8U, 9U}) {
        SCOPED_TRACE(i);
        EXPECT_GT(values[i].x, 0.0F);
        EXPECT_GT(values[i].y, 0.0F);
    }
}

/// The direction of anisotropy in the shipped BSDF. A light leaves the mirror direction of an
/// anisotropic surface by 15 degrees, once toward the tangent of the frame and once toward its
/// bitangent. The response toward the tangent (mode 12) must be far larger than the one toward
/// the bitangent (13): OpenPBR stretches the NDF, and with it the highlight, along the tangent
/// (`alphaX` is the larger roughness), and puts the grooves along the bitangent. Without
/// anisotropy both tilts give the same response (14, 15). With the frame turned by 90 degrees
/// (16, 17) the same two world directions swap roles, so the stretch follows the frame, not
/// the world.
TEST_F(ShaderDispatchFixture, BsdfMeasure_AnisotropyStretchesTheHighlightAlongTheTangent) {
    const auto values = runShader("bsdf_measure_test.spv", 18U);
    ASSERT_EQ(values.size(), 18U);
    const auto specular = [&values](std::size_t mode) { return values[mode].x; };
    ASSERT_GT(specular(12), 0.0F);
    ASSERT_GT(specular(13), 0.0F);
    RecordProperty("response_toward_tangent", specular(12));
    RecordProperty("response_toward_bitangent", specular(13));
    RecordProperty("response_ratio", specular(12) / specular(13));
    EXPECT_GT(specular(12) / specular(13), 100.0F);
    EXPECT_NEAR(specular(14) / specular(15), 1.0F, 1.0e-3F);
    EXPECT_NEAR(specular(17) / specular(12), 1.0F, 1.0e-3F);
    EXPECT_NEAR(specular(16) / specular(13), 1.0F, 1.0e-3F);
}
