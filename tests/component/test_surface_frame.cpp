#include <cmath>
#include <cstddef>
#include <gtest/gtest.h>
#include <slang-math/slang-math.hpp>
#include <string>
#include <vector>

#include "fixtures/ShaderDispatchFixture.hpp"

namespace {

/// The six vectors of one frame, in the order surface_frame_test.slang writes them.
constexpr std::size_t kVectorsPerFrame = 6U;
constexpr std::size_t kModes = 8U;
constexpr std::size_t kStorageModes = 11U; // 8/9/10: front/back/zero emission predicate probes

struct Frame {
    sm::float3 normal;
    sm::float3 tangent;
    sm::float3 bitangent;
    sm::float3 coatNormal;
    sm::float3 coatTangent;
    sm::float3 coatBitangent;
};

[[nodiscard]] sm::float3 vectorOf(const std::vector<sm::float4>& values, std::size_t mode, std::size_t k) {
    const sm::float4& v = values[mode * kVectorsPerFrame + k];
    return sm::float3{v.x, v.y, v.z};
}

[[nodiscard]] Frame frameOf(const std::vector<sm::float4>& values, std::size_t mode) {
    return {vectorOf(values, mode, 0U),
            vectorOf(values, mode, 1U),
            vectorOf(values, mode, 2U),
            vectorOf(values, mode, 3U),
            vectorOf(values, mode, 4U),
            vectorOf(values, mode, 5U)};
}

[[nodiscard]] float dot3(sm::float3 a, sm::float3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

[[nodiscard]] float length3(sm::float3 a) {
    return std::sqrt(dot3(a, a));
}

[[nodiscard]] sm::float3 cross3(sm::float3 a, sm::float3 b) {
    return sm::float3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

/// A right-handed orthonormal frame: every vector of unit length, tangent and bitangent in the
/// plane of the normal, bitangent = normal x tangent.
void expectOrthonormalFrame(const char* what, sm::float3 normal, sm::float3 tangent, sm::float3 bitangent) {
    SCOPED_TRACE(what);
    EXPECT_NEAR(length3(normal), 1.0F, 1.0e-4F);
    EXPECT_NEAR(length3(tangent), 1.0F, 1.0e-4F);
    EXPECT_NEAR(length3(bitangent), 1.0F, 1.0e-4F);
    EXPECT_NEAR(dot3(normal, tangent), 0.0F, 1.0e-4F);
    EXPECT_NEAR(dot3(normal, bitangent), 0.0F, 1.0e-4F);
    EXPECT_NEAR(dot3(tangent, bitangent), 0.0F, 1.0e-4F);
    EXPECT_NEAR(dot3(cross3(normal, tangent), bitangent), 1.0F, 1.0e-4F);
}

/// Two unit vectors point the same way.
void expectDirection(const char* what, sm::float3 expected, sm::float3 actual) {
    SCOPED_TRACE(what);
    EXPECT_NEAR(dot3(expected, actual), 1.0F, 1.0e-4F);
}

} // namespace

/// The frame of the anisotropy that applyOpenPbrTextureMaps builds. OpenPBR's reference
/// implementation binds `geometry_tangent` and `geometry_coat_tangent` to the tangent of the
/// geometry (Tworld in open_pbr_surface.mtlx): the direction of increasing u, projected into
/// the plane of the normal (spec, "Normal maps and tangents"). The frame used to be built from
/// the normal alone, so the direction of an anisotropic highlight ignored the texture
/// coordinates unless a tangent map was bound, and the coat inherited a base tangent map.
/// Modes 1, 2, 3, 6 and 7 fail against that shader.
TEST_F(ShaderDispatchFixture, SurfaceFrame_DefaultTangentIsTheTangentOfTheTextureCoordinates) {
    const auto values = runShader("surface_frame_test.spv", kStorageModes * kVectorsPerFrame);
    ASSERT_EQ(values.size(), kStorageModes * kVectorsPerFrame);
    const sm::float3 x{1.0F, 0.0F, 0.0F};
    const sm::float3 y{0.0F, 1.0F, 0.0F};
    const sm::float3 z{0.0F, 0.0F, 1.0F};
    const sm::float3 nx{-1.0F, 0.0F, 0.0F};
    const sm::float3 diagonal{0.70710678F, 0.70710678F, 0.0F};
    const sm::float3 antiDiagonal{-0.70710678F, 0.70710678F, 0.0F};
    const sm::float3 tilted{0.0F, 0.6F, 0.8F};
    // +Y projected into the plane of `tilted`: Y - tilted * (tilted . Y), normalized.
    const sm::float3 tiltedTangent{0.0F, 0.8F, -0.6F};
    const sm::float3 tiltedBitangent{-1.0F, 0.0F, 0.0F};
    const sm::float3 tiltedCoatNormal{-0.70710678F, 0.0F, 0.70710678F};
    const sm::float3 tiltedCoatBitangent{-0.70710678F, 0.0F, -0.70710678F};
    const sm::float3 anyDirection{0.0F, 0.0F, 0.0F};
    //                     normal  tangent     bitangent          coat normal      coat tangent     coat bitangent
    const Frame expected[kModes] = {
        {z, x, y, z, x, y},                                     // 0: texture tangent +X
        {z, y, nx, z, y, nx},                                   // 1: texture tangent +Y
        {z, diagonal, antiDiagonal, z, diagonal, antiDiagonal}, // 2: texture tangent between +X and +Y
        {tilted, tiltedTangent, tiltedBitangent, tilted, tiltedTangent, tiltedBitangent}, // 3: tilted normal
        {z, anyDirection, anyDirection, z, anyDirection, anyDirection}, // 4: tangent parallel to the normal
        {z, anyDirection, anyDirection, z, anyDirection, anyDirection}, // 5: no tangent
        {z, y, nx, tiltedCoatNormal, y, tiltedCoatBitangent},           // 6: coat normal map
        {z, y, nx, z, x, y},                                            // 7: base tangent map stores +v
    };
    for (std::size_t mode = 0; mode < kModes; ++mode) {
        SCOPED_TRACE("mode " + std::to_string(mode));
        const Frame frame = frameOf(values, mode);
        expectOrthonormalFrame("base", frame.normal, frame.tangent, frame.bitangent);
        expectOrthonormalFrame("coat", frame.coatNormal, frame.coatTangent, frame.coatBitangent);
        const Frame& want = expected[mode];
        expectDirection("normal", want.normal, frame.normal);
        expectDirection("coat normal", want.coatNormal, frame.coatNormal);
        if (mode == 4U || mode == 5U) {
            // OpenPBR lets the implementation take any unit vector orthogonal to the normal.
            // The coat normal is the base normal, so the coat shares the base frame.
            expectDirection("coat tangent = base tangent", frame.tangent, frame.coatTangent);
            expectDirection("coat bitangent = base bitangent", frame.bitangent, frame.coatBitangent);
            continue;
        }
        expectDirection("tangent", want.tangent, frame.tangent);
        expectDirection("bitangent", want.bitangent, frame.bitangent);
        expectDirection("coat tangent", want.coatTangent, frame.coatTangent);
        expectDirection("coat bitangent", want.coatBitangent, frame.coatBitangent);
    }
}

TEST_F(ShaderDispatchFixture, SurfaceFrame_EmissionReplayPredicateUsesResolvedFrontFaceEdf) {
    const auto values = runShader("surface_frame_test.spv", kStorageModes * kVectorsPerFrame);
    ASSERT_EQ(values.size(), kStorageModes * kVectorsPerFrame);
    EXPECT_FLOAT_EQ(values[8U * kVectorsPerFrame].x, 1.0F) << "positive front-face EDF needs replay";
    EXPECT_FLOAT_EQ(values[9U * kVectorsPerFrame].x, 0.0F) << "back-face EDF is not emitted";
    EXPECT_FLOAT_EQ(values[10U * kVectorsPerFrame].x, 0.0F) << "zero luminance needs no EDF replay";
}
