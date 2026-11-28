#include <array>
#include <cmath>
#include <cstdint>
#include <gtest/gtest.h>
#include <iomanip>
#include <iostream>
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
    // Conditional value of actual delta transmission samples. Fractional smooth (27) and
    // thin-walled (28) materials have specular=1, transmission=.5, total=1.5. At normal
    // incidence Fresnel(1.5)=.04, so the value/selection-PMF ratio before transport is
    // total*(1-F)=1.44. Camera-radiance transport through a solid interface divides by
    // eta^2 (PBRT-v4 DielectricBxDF), making mode 27 exactly .64. A thin wall returns to
    // the same exterior medium and has no net IOR transport factor, so mode 28 stays 1.44.
    // Full smooth glass (29) keeps its Fresnel ownership and conditional value 1/eta^2.
    for (uint32_t mode : {27U, 28U}) {
        SCOPED_TRACE(mode);
        ASSERT_GT(values[mode].y, 0.0F) << "no transmitted sample was measured";
        const float expected = mode == 27U ? 0.64F : 1.44F;
        EXPECT_NEAR(values[mode].x, expected, 1.0e-5F);
    }
    ASSERT_GT(values[29].y, 0.0F);
    EXPECT_NEAR(values[29].x, 1.0F / 2.25F, 1.0e-6F)
        << "full smooth glass radiance transport matches PBRT smooth and rough limits";
}

TEST_F(ShaderDispatchFixture, BsdfMeasure_BulkSubsurfaceEntryDoesNotDuplicateDielectricReflection) {
    const auto values = runShader("bsdf_measure_test.spv", 49U);
    ASSERT_EQ(values.size(), 49U);
    ASSERT_GT(values[48].y, 0.0F) << "bulk-SSS entry was not exercised";
    // specular=1 and SSS=1 give total=2. At normal incidence F=.04 and eta=1.5,
    // so the conditional substrate entry value is total*(1-F)/eta^2=.853333...
    EXPECT_NEAR(values[48].x, 0.85333333F, 1.0e-6F);
    EXPECT_EQ(values[48].z, 0.0F) << "the separate dielectric reflection lobe already owns external Fresnel reflection";
}

TEST_F(ShaderDispatchFixture, BsdfMeasure_BulkSubsurfaceColorMapsToSingleScatterAlbedo) {
    const auto values = runShader("bsdf_measure_test.spv", 48U);
    ASSERT_EQ(values.size(), 48U);
    ASSERT_GT(values[47].w, 0.0F) << "bulk-SSS medium entry was not exercised";
    // OpenPBR 1.1.2 subsurface van de Hulst inversion; identical to Adobe Eclair.
    EXPECT_NEAR(values[47].x, 0.99292494F, 2.0e-6F); // binary32 evaluation of the spec polynomial
    EXPECT_NEAR(values[47].y, 0.93228703F, 2.0e-6F);
    EXPECT_NEAR(values[47].z, 0.67898877F, 2.0e-6F);
}

TEST_F(ShaderDispatchFixture, BsdfMeasure_RoughTransmissionTirIsNotADeltaEvent) {
    const auto values = runShader("bsdf_measure_test.spv", 43U);
    ASSERT_EQ(values.size(), 43U);
    EXPECT_GT(values[42].y, 0.0F) << "rough exiting sampler was not exercised";
    EXPECT_EQ(values[42].z, 0.0F) << "rough TIR sampler produced a non-finite result";
    EXPECT_EQ(values[42].x, 0.0F)
        << "Adobe/PBRT reject failed rough refraction; a continuous rough direction is never delta";
}

TEST_F(ShaderDispatchFixture, BsdfMeasure_MediumExitInvertsEntryRadianceTransportOnlyOnExit) {
    const auto values = runShader("bsdf_measure_test.spv", 42U);
    ASSERT_EQ(values.size(), 42U);
    ASSERT_GT(values[40].y, 0.0F) << "normal-incidence exit branch was not exercised";
    EXPECT_NEAR(values[40].x, 2.25F, 1.0e-6F) << "air<-glass exit applies eta^2";
    if (values[40].w > 0.0F) {
        EXPECT_NEAR(values[40].z, 1.0F, 1.0e-6F) << "ordinary Fresnel reflection has no transport scale";
    }
    EXPECT_EQ(values[41].y, 0.0F) << "60 degrees inside IOR 1.5 must be TIR";
    EXPECT_NEAR(values[41].x, 1.0F, 1.0e-6F) << "TIR has no transmission transport scale";
    EXPECT_NEAR(values[41].z, -0.5F, 1.0e-6F);
}

TEST_F(ShaderDispatchFixture, BsdfMeasure_AdobeEclairDifferentialProbe) {
    const auto values = runShader("bsdf_measure_test.spv", 47U);
    ASSERT_EQ(values.size(), 47U);
    // Adobe openpbr-bsdf c91aad1d1ce1693e803f039d7c92c2965c4eb013, OpenPBR 1.1.1.
    // Full-material values include Adobe's production multiple-scattering lobes and are a
    // differential diagnostic, not an assertion that two different sampling PMFs must match.
    constexpr std::array<std::array<float, 3>, 14> adobeCos{{
        {8.850337982F, 8.850337982F, 8.850337982F},
        {0.2080281228F, 0.2080281228F, 0.2080281228F},
        {1718.873169F, 1718.873169F, 1718.873169F},
        {1.989436746F, 1.989436746F, 1.989436746F},
        {988.3170166F, 988.3170166F, 988.3170166F},
        {3.530912638F, 3.530912638F, 3.530912638F},
        {5.570073605F, 5.629783154F, 5.568669319F},
        {0.004202701151F, 0.03650648892F, 0.002792584011F},
        {219.5317841F, 333.3610535F, 219.5317841F},
        {5.608922005F, 5.69054985F, 5.608922005F},
        {0.02993844822F, 0.02993844822F, 0.02993844822F},
        {0.02948963828F, 0.02948963828F, 0.02948963828F},
        {15.41597557F, 5.915137768F, 2.072684526F},
        {0.01874166541F, 0.07671923935F, 0.01372060925F},
    }};
    constexpr std::array<float, 14> lightCos{0.5F,
                                             0.5F,
                                             1.0F,
                                             1.0F,
                                             0.88168347F,
                                             0.70710678F,
                                             0.70710678F,
                                             1.0F,
                                             0.88168347F,
                                             0.70710678F,
                                             0.5F,
                                             1.0F,
                                             0.5F,
                                             1.0F};
    std::cout << std::setprecision(9) << "mode,adobe_cos,harmonia_cos,ratio,harmonia_pdf\n";
    for (std::size_t i = 0; i < adobeCos.size(); ++i) {
        const std::size_t mode = i < 10U ? i + 30U : i + 33U;
        const auto& v = values[mode];
        const float actual = v.y * lightCos[i];
        const float expected = adobeCos[i][1];
        std::cout << i << ',' << expected << ',' << actual << ',' << actual / expected << ',' << v.w << '\n';
        EXPECT_TRUE(std::isfinite(actual));
        EXPECT_TRUE(std::isfinite(v.w));
        if (i < 2U || i == 3U || i == 5U || i == 6U || i >= 10U) {
            // Comparable dielectric/metal/fuzz/coated-opaque cases allow 5% for documented
            // multiple-scattering and layer-approximation differences. Transmission transport,
            // Adobe's view-dependent coat absorption, and its separate refraction-IOR convention
            // are intentionally diagnostics rather than equality requirements.
            EXPECT_NEAR(actual, expected, expected * 0.05F) << "Adobe-comparable case " << i;
        }
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
