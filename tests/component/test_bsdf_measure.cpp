#include <array>
#include <cstdint>
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

TEST_F(ShaderDispatchFixture, BsdfMeasure_PartialTransmissionMatchesOpenPbrSubstrateMixture) {
    const auto values = runShader("bsdf_measure_test.spv", 30U);
    ASSERT_EQ(values.size(), 30U);
    // Independent scalar evaluation of OpenPBR 1.1.2:
    // D = 1-metalness, O = D * (1-transmission),
    // diffuse = base_weight * O * (1-subsurface), SSS = O * subsurface,
    // transmission = D * transmission, and reflection = D above the entire substrate.
    // base_weight belongs to the diffuse/metal closures, not to SSS or the dielectric interface.
    // At full nonmetallic transmission only the smooth optimized delta-glass path owns R+T together;
    // rough full glass (mode 24) retains its ordinary microfacet reflection.
    constexpr std::array<std::array<float, 4>, 7> expected{{
        {0.375F, 0.375F, 1.00F, 0.25F},
        {0.250F, 0.250F, 1.00F, 0.50F},
        {0.125F, 0.125F, 1.00F, 0.75F},
        {0.000F, 0.000F, 0.00F, 1.00F},
        {0.1875F, 0.1875F, 0.75F, 0.375F},
        {0.000F, 0.250F, 1.00F, 0.50F},
        {0.000F, 0.000F, 1.00F, 1.00F},
    }};
    for (std::size_t mode = 18; mode < expected.size() + 18; ++mode) {
        SCOPED_TRACE(mode);
        const auto& e = expected[mode - 18];
        EXPECT_NEAR(values[mode].x, e[0], 1.0e-6F) << "diffuse";
        EXPECT_NEAR(values[mode].y, e[1], 1.0e-6F) << "subsurface";
        EXPECT_NEAR(values[mode].z, e[2], 1.0e-6F) << "specular reflection";
        EXPECT_NEAR(values[mode].w, e[3], 1.0e-6F) << "transmission";
    }
}

TEST_F(ShaderDispatchFixture, BsdfMeasure_BaseWeightDoesNotLeakIntoFractionalDielectricReflection) {
    const auto values = runShader("bsdf_measure_test.spv", 30U);
    ASSERT_EQ(values.size(), 30U);
    ASSERT_GT(values[26].y, 1.0e-8F);
    // Mode 25: metalness=.5, base_weight=0. Mode 26: pure dielectric reference.
    // With no metal closure present, OpenPBR's mixture retains exactly half the dielectric eval.
    EXPECT_NEAR(values[25].x, 0.5F * values[26].x, 1.0e-6F);
    EXPECT_NEAR(values[25].y, 0.5F * values[26].y, 1.0e-6F);
    EXPECT_NEAR(values[25].z, 0.5F * values[26].z, 1.0e-6F);
}

TEST_F(ShaderDispatchFixture, BsdfMeasure_FractionalDeltaTransmissionCarriesValueOverSelectionPmf) {
    const auto values = runShader("bsdf_measure_test.spv", 30U);
    ASSERT_EQ(values.size(), 30U);
    // Normal-incidence Fresnel(1.5) = 0.04. For specular=1, T=.5, total=1.5, the
    // value/selection-PMF ratio is total*(1-F) = 1.44, not total/T = 3.
    for (uint32_t mode : {27U, 28U}) {
        SCOPED_TRACE(mode);
        ASSERT_GT(values[mode].y, 0.0F) << "no transmitted sample was measured";
        EXPECT_NEAR(values[mode].x, 1.44F, 1.0e-5F);
    }
    ASSERT_GT(values[29].y, 0.0F);
    EXPECT_NEAR(values[29].x, 1.0F, 1.0e-6F) << "full smooth glass Fresnel ownership is unchanged";
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
