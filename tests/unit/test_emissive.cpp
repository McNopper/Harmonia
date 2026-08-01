#include <cmath>
#include <cstdint>
#include <gtest/gtest.h>
#include <numbers>
#include <slang-math/slang-math.hpp>
#include <vector>

#include "harmonia/GpuTypes.hpp"
#include "harmonia/scene/EmissiveBuilder.hpp"
#include "harmonia/scene/Geometry.hpp"

namespace {

constexpr float kEpsilon = 1.0e-6F;

// The shader-side tag for an analytic sphere emitter (gpu_types.slang: kEmissiveSphere).
constexpr std::uint32_t kEmissiveSphere = 1u;
constexpr std::uint32_t kEmissiveTriangle = 0u;

// A valid selection CDF is non-decreasing, starts > 0, ends at exactly 1, in [0,1].
void expectValidCdf(const std::vector<float>& cdf) {
    ASSERT_FALSE(cdf.empty());
    EXPECT_GT(cdf.front(), 0.0F);
    EXPECT_NEAR(cdf.back(), 1.0F, kEpsilon);
    for (const float v : cdf) {
        EXPECT_GE(v, 0.0F);
        EXPECT_LE(v, 1.0F + kEpsilon);
    }
    for (std::size_t i = 1; i < cdf.size(); ++i) {
        EXPECT_GE(cdf[i] + kEpsilon, cdf[i - 1]) << "not non-decreasing at " << i;
    }
}

// Object→world matrix for an instance placement, mirroring what buildEmissiveData feeds
// appendSphereEmissive (InstanceRecord::xform.matrix()).
sm::float4x4
placement(sm::float3 translation, sm::float3 scale, sm::quaternion rotation = sm::identity<sm::quaternion>()) {
    harmonia::Xform xform;
    xform.translation = translation;
    xform.rotation = rotation;
    xform.scale = scale;
    return xform.matrix();
}

// Inverse-CDF lookup with a stratified variate, matching the GPU binary search's semantics
// (first index whose cumulative probability reaches u).
std::vector<float> cdfSelectionFrequencies(const std::vector<float>& cdf, std::uint32_t steps) {
    std::vector<std::uint32_t> hits(cdf.size(), 0u);
    for (std::uint32_t s = 0; s < steps; ++s) {
        const float u = (static_cast<float>(s) + 0.5F) / static_cast<float>(steps);
        std::size_t idx = 0;
        while (idx + 1u < cdf.size() && u > cdf[idx]) {
            ++idx;
        }
        ++hits[idx];
    }
    std::vector<float> freq(cdf.size(), 0.0F);
    for (std::size_t i = 0; i < cdf.size(); ++i) {
        freq[i] = static_cast<float>(hits[i]) / static_cast<float>(steps);
    }
    return freq;
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
    for (std::uint32_t i = 0; i < 4u; ++i) {
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

    const std::uint32_t steps = 10000u;
    std::vector<std::uint32_t> hits(cdf.size(), 0u);
    for (std::uint32_t s = 0; s < steps; ++s) {
        const float u = (static_cast<float>(s) + 0.5F) / static_cast<float>(steps); // stratified in (0,1)
        std::uint32_t idx = 0u;
        while (idx + 1u < cdf.size() && u > cdf[idx]) {
            ++idx;
        }
        ++hits[idx];
    }
    for (std::size_t i = 0; i < cdf.size(); ++i) {
        const float expected = power[i] / total;
        const float actual = static_cast<float>(hits[i]) / static_cast<float>(steps);
        EXPECT_NEAR(actual, expected, 1.0e-3F) << "emitter " << i;
    }
}

// ---------------------------------------------------------------------------
// Analytic sphere emitters (appendSphereEmissive)
//
// A Sphere geometry can only be created against a Vulkan device, so the packing
// rules live in the device-free appendSphereEmissive that buildEmissiveData calls
// for every Sphere instance — these tests exercise exactly that path.
// ---------------------------------------------------------------------------

TEST(SphereEmissive, TaggedWithTheSphereKind) {
    // The tagged union must announce itself as a sphere; the GPU dispatches the whole
    // cone-sampling branch on kind_pad.x == kEmissiveSphere.
    harmonia::EmissiveData data;
    const harmonia::SphereEmissive packed = harmonia::appendSphereEmissive(
        data, 0.7F, placement({0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}), {1.0F, 1.0F, 1.0F});

    ASSERT_EQ(data.triangles.size(), 1u);
    EXPECT_EQ(data.triangles.front().kind_pad.x, kEmissiveSphere);
    EXPECT_EQ(data.triangles.front().kind_pad.x, static_cast<std::uint32_t>(harmonia::EmissiveKind::Sphere));
    EXPECT_EQ(packed.entry.kind_pad.x, kEmissiveSphere);
}

TEST(SphereEmissive, PacksWorldCentreAndRadius) {
    // v0_area is overloaded for spheres: xyz = world centre, w = world radius. The shader
    // reads it back verbatim, so the round-trip through the instance transform must be exact.
    harmonia::EmissiveData data;
    harmonia::appendSphereEmissive(data, 0.7F, placement({1.0F, 2.0F, 3.0F}, {2.0F, 2.0F, 2.0F}), {1.0F, 1.0F, 1.0F});

    ASSERT_EQ(data.triangles.size(), 1u);
    const harmonia::GpuEmissiveTriangle& e = data.triangles.front();
    EXPECT_NEAR(e.v0_area.x, 1.0F, kEpsilon);
    EXPECT_NEAR(e.v0_area.y, 2.0F, kEpsilon);
    EXPECT_NEAR(e.v0_area.z, 3.0F, kEpsilon);
    EXPECT_NEAR(e.v0_area.w, 1.4F, 1.0e-5F); // 0.7 × uniform scale 2
    // Emission rides in the three w-channels; edge1/edge2/normal xyz are unused.
    EXPECT_NEAR(e.edge1_emitR.w, 1.0F, kEpsilon);
    EXPECT_NEAR(e.edge2_emitG.w, 1.0F, kEpsilon);
    EXPECT_NEAR(e.normal_emitB.w, 1.0F, kEpsilon);
}

TEST(SphereEmissive, RadiusIsRotationInvariant) {
    // A rotation is an isometry: it may not change the world radius by any amount, and in
    // particular may not trip the non-uniform-scale detection.
    // Unit quaternion: 0.9 rad about normalize(1, 2, -0.5) — a tilt on every axis.
    const sm::quaternion tilt{0.189834F, 0.379668F, -0.094917F, 0.900447F};
    harmonia::EmissiveData data;
    const harmonia::SphereEmissive packed = harmonia::appendSphereEmissive(
        data, 0.7F, placement({0.0F, 1.0F, 0.0F}, {3.0F, 3.0F, 3.0F}, tilt), {1.0F, 1.0F, 1.0F});

    EXPECT_TRUE(packed.uniform);
    EXPECT_NEAR(packed.radius, 2.1F, 1.0e-5F);
}

TEST(SphereEmissive, PowerIsSphereAreaTimesLuminance) {
    // Power must be 4πr² × luminance(Le) — the same area × luminance unit the triangle
    // branch produces, otherwise the shared CDF ranks the two emitter kinds against
    // incompatible numbers. Rec.2020 luminance of white is exactly 1.
    harmonia::EmissiveData data;
    const harmonia::SphereEmissive packed = harmonia::appendSphereEmissive(
        data, 0.5F, placement({0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}), {2.0F, 2.0F, 2.0F});

    const float expected = 4.0F * std::numbers::pi_v<float> * 0.5F * 0.5F * 2.0F;
    ASSERT_EQ(data.power.size(), 1u);
    EXPECT_NEAR(data.power.front(), expected, 1.0e-5F);
    EXPECT_NEAR(packed.power, expected, 1.0e-5F);
}

TEST(SphereEmissive, PowerUsesRec2020Luminance) {
    // Non-white emission is weighted by the same Rec.2020 coefficients as the triangles.
    harmonia::EmissiveData data;
    harmonia::appendSphereEmissive(data, 1.0F, placement({0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}), {1.0F, 0.0F, 0.0F});

    ASSERT_EQ(data.power.size(), 1u);
    EXPECT_NEAR(data.power.front(), 4.0F * std::numbers::pi_v<float> * 0.2627F, 1.0e-5F);
}

TEST(SphereEmissive, IsIncludedInTheEmitterCount) {
    // emissiveTriangleCount is triangles.size(); a sphere must occupy one slot in BOTH
    // arrays or the CDF and the buffer fall out of step and NEE indexes the wrong emitter.
    harmonia::EmissiveData data;
    data.triangles.push_back(harmonia::GpuEmissiveTriangle{
        .v0_area = sm::float4{0.0F, 0.0F, 0.0F, 1.0F},
        .edge1_emitR = sm::float4{1.0F, 0.0F, 0.0F, 1.0F},
        .edge2_emitG = sm::float4{0.0F, 1.0F, 0.0F, 1.0F},
        .normal_emitB = sm::float4{0.0F, 0.0F, 1.0F, 1.0F},
        .kind_pad = sm::uint4{kEmissiveTriangle, 0u, 0u, 0u},
    });
    data.power.push_back(1.0F);

    harmonia::appendSphereEmissive(data, 0.7F, placement({0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}), {1.0F, 1.0F, 1.0F});

    ASSERT_EQ(data.triangles.size(), 2u);
    ASSERT_EQ(data.power.size(), data.triangles.size());
    EXPECT_EQ(data.triangles[0].kind_pad.x, kEmissiveTriangle);
    EXPECT_EQ(data.triangles[1].kind_pad.x, kEmissiveSphere);
    EXPECT_GT(data.power[1], 0.0F);
}

TEST(SphereEmissive, ZeroScaleIsSkipped) {
    // A collapsed instance has no surface to sample; it must not enter the buffer at all
    // (a radius-0 entry would make the cone solid angle 0 and the pdf infinite).
    harmonia::EmissiveData data;
    const harmonia::SphereEmissive packed = harmonia::appendSphereEmissive(
        data, 0.7F, placement({0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F}), {1.0F, 1.0F, 1.0F});

    EXPECT_EQ(packed.radius, 0.0F);
    EXPECT_TRUE(data.triangles.empty());
    EXPECT_TRUE(data.power.empty());
}

TEST(SphereEmissive, NonUniformScaleIsSkippedAsUnsupported) {
    // The primitive is intersected in object space, so a non-uniform scale is a genuine
    // ellipsoid the uniform-cone sphere sampler cannot represent. Approximating it with an
    // enclosing sphere would hand back a lit point, a normal and a shadow-ray tMax belonging
    // to different geometry than the traced surface, so the emitter is skipped (and reported
    // via the Logger warning in buildEmissiveData) rather than silently mis-sampled.
    harmonia::EmissiveData data;
    const harmonia::SphereEmissive packed = harmonia::appendSphereEmissive(
        data, 1.0F, placement({0.0F, 0.0F, 0.0F}, {1.0F, 3.0F, 2.0F}), {1.0F, 1.0F, 1.0F});

    EXPECT_FALSE(packed.uniform);
    EXPECT_FALSE(packed.appended);
    EXPECT_NEAR(packed.axisLengths.x, 1.0F, 1.0e-5F);
    EXPECT_NEAR(packed.axisLengths.y, 3.0F, 1.0e-5F);
    EXPECT_NEAR(packed.axisLengths.z, 2.0F, 1.0e-5F);
    EXPECT_TRUE(data.triangles.empty());
    EXPECT_TRUE(data.power.empty());
}

TEST(SphereEmissive, UniformScaleIsNotFlagged) {
    harmonia::EmissiveData data;
    const harmonia::SphereEmissive packed = harmonia::appendSphereEmissive(
        data, 0.25F, placement({-1.0F, 0.5F, 2.0F}, {4.0F, 4.0F, 4.0F}), {1.0F, 1.0F, 1.0F});

    EXPECT_TRUE(packed.uniform);
    EXPECT_NEAR(packed.radius, 1.0F, 1.0e-5F);
}

TEST(EmissiveCdf, MixedSphereAndTriangleSelectProportionallyToPower) {
    // The single power CDF ranks spheres and triangles together, so their power expressions
    // must share a unit (4πr²·lum vs area·lum). Build a mixed set the way buildEmissiveData
    // does and check the selection frequencies land on power_i / Σpower.
    harmonia::EmissiveData data;

    // Two unit-luminance triangles of area 2 and 6 (pushed exactly as the triangle branch does).
    const std::vector<float> triAreas{2.0F, 6.0F};
    for (const float area : triAreas) {
        data.triangles.push_back(harmonia::GpuEmissiveTriangle{
            .v0_area = sm::float4{0.0F, 0.0F, 0.0F, area},
            .edge1_emitR = sm::float4{1.0F, 0.0F, 0.0F, 1.0F},
            .edge2_emitG = sm::float4{0.0F, 1.0F, 0.0F, 1.0F},
            .normal_emitB = sm::float4{0.0F, 0.0F, 1.0F, 1.0F},
            .kind_pad = sm::uint4{kEmissiveTriangle, 0u, 0u, 0u},
        });
        data.power.push_back(area); // area × luminance(white) == area
    }
    // One unit-luminance sphere of world radius 1 → power 4π ≈ 12.566.
    harmonia::appendSphereEmissive(data, 1.0F, placement({0.0F, 3.0F, 0.0F}, {1.0F, 1.0F, 1.0F}), {1.0F, 1.0F, 1.0F});

    ASSERT_EQ(data.triangles.size(), 3u);
    ASSERT_EQ(data.power.size(), 3u);

    const std::vector<float> cdf = harmonia::buildEmissiveCdf(data.power);
    ASSERT_EQ(cdf.size(), 3u);
    expectValidCdf(cdf);

    const float total = data.power[0] + data.power[1] + data.power[2];
    const std::vector<float> freq = cdfSelectionFrequencies(cdf, 100000u);
    for (std::size_t i = 0; i < cdf.size(); ++i) {
        EXPECT_NEAR(freq[i], data.power[i] / total, 1.0e-3F) << "emitter " << i;
    }
    // The sphere carries the most power here, so it must also be selected most often —
    // the property the 4π factor exists to preserve.
    EXPECT_GT(freq[2], freq[1]);
    EXPECT_GT(freq[2], freq[0]);
}
