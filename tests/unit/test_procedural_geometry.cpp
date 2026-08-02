#include <algorithm>
#include <array>
#include <cmath>
#include <gtest/gtest.h>
#include <limits>
#include <slang-math/slang-math.hpp>

#include "harmonia/scene/Geometry.hpp"
#include "harmonia/scene/ProceduralGeometry.hpp"
#include "harmonia/utils/Math.hpp"

namespace {
constexpr float kEpsilon = 1.0e-5F;

[[nodiscard]] bool matchesAnyDirection(sm::float3 value, const std::array<sm::float3, 6>& directions) noexcept {
    return std::any_of(directions.begin(), directions.end(), [&](const sm::float3 direction) {
        return sm::length(value - direction) <= 1.0e-4F;
    });
}
} // namespace

TEST(ProceduralGeometry, MakeBoxIdentityProducesValidBoxMesh) {
    const harmonia::MeshData mesh =
        harmonia::ProceduralGeometry::makeBox(sm::float3(1.0F, 1.0F, 1.0F), sm::float4x4(1.0F));

    EXPECT_TRUE(mesh.vertices.size() == 24U || mesh.vertices.size() == 8U);
    EXPECT_EQ(mesh.indices.size(), 36U);

    const std::array axisDirections{
        sm::float3(1.0F, 0.0F, 0.0F),
        sm::float3(-1.0F, 0.0F, 0.0F),
        sm::float3(0.0F, 1.0F, 0.0F),
        sm::float3(0.0F, -1.0F, 0.0F),
        sm::float3(0.0F, 0.0F, 1.0F),
        sm::float3(0.0F, 0.0F, -1.0F),
    };

    for (const harmonia::GpuVertex& vertex : mesh.vertices) {
        EXPECT_NEAR(sm::length(vertex.normal), 1.0F, kEpsilon);
        EXPECT_TRUE(matchesAnyDirection(vertex.normal, axisDirections));
        EXPECT_GE(vertex.position.x, -1.0F - kEpsilon);
        EXPECT_GE(vertex.position.y, -1.0F - kEpsilon);
        EXPECT_GE(vertex.position.z, -1.0F - kEpsilon);
        EXPECT_LE(vertex.position.x, 1.0F + kEpsilon);
        EXPECT_LE(vertex.position.y, 1.0F + kEpsilon);
        EXPECT_LE(vertex.position.z, 1.0F + kEpsilon);
        EXPECT_GE(vertex.uv.x, 0.0F);
        EXPECT_GE(vertex.uv.y, 0.0F);
        EXPECT_LE(vertex.uv.x, 1.0F);
        EXPECT_LE(vertex.uv.y, 1.0F);
    }
}

TEST(ProceduralGeometry, MakeBoxRotationTransformsNormals) {
    const sm::float4x4 rotation = harmonia::Math::makeRotationY(harmonia::Math::kPi * 0.5F);
    const harmonia::MeshData mesh = harmonia::ProceduralGeometry::makeBox(sm::float3(1.0F, 1.0F, 1.0F), rotation);
    const sm::float3x3 normalTransform = sm::toFloat3x3(rotation);

    const std::array expectedDirections{
        sm::normalize(normalTransform * sm::float3(1.0F, 0.0F, 0.0F)),
        sm::normalize(normalTransform * sm::float3(-1.0F, 0.0F, 0.0F)),
        sm::normalize(normalTransform * sm::float3(0.0F, 1.0F, 0.0F)),
        sm::normalize(normalTransform * sm::float3(0.0F, -1.0F, 0.0F)),
        sm::normalize(normalTransform * sm::float3(0.0F, 0.0F, 1.0F)),
        sm::normalize(normalTransform * sm::float3(0.0F, 0.0F, -1.0F)),
    };

    for (const harmonia::GpuVertex& vertex : mesh.vertices) {
        EXPECT_NEAR(sm::length(vertex.normal), 1.0F, kEpsilon);
        EXPECT_TRUE(matchesAnyDirection(vertex.normal, expectedDirections));
    }
}

TEST(ProceduralGeometry, MakeSphereAabbAtOriginMatchesUnitSphere) {
    const auto aabb = harmonia::ProceduralGeometry::makeSphereAabb(sm::float3(0.0F, 0.0F, 0.0F), 1.0F);
    EXPECT_NEAR(aabb.min.x, -1.0F, kEpsilon);
    EXPECT_NEAR(aabb.min.y, -1.0F, kEpsilon);
    EXPECT_NEAR(aabb.min.z, -1.0F, kEpsilon);
    EXPECT_NEAR(aabb.max.x, 1.0F, kEpsilon);
    EXPECT_NEAR(aabb.max.y, 1.0F, kEpsilon);
    EXPECT_NEAR(aabb.max.z, 1.0F, kEpsilon);
}

TEST(ProceduralGeometry, MakeSphereAabbWithOffsetMatchesExpectedBounds) {
    const auto aabb = harmonia::ProceduralGeometry::makeSphereAabb(sm::float3(1.0F, 2.0F, 3.0F), 0.5F);
    EXPECT_NEAR(aabb.min.x, 0.5F, kEpsilon);
    EXPECT_NEAR(aabb.min.y, 1.5F, kEpsilon);
    EXPECT_NEAR(aabb.min.z, 2.5F, kEpsilon);
    EXPECT_NEAR(aabb.max.x, 1.5F, kEpsilon);
    EXPECT_NEAR(aabb.max.y, 2.5F, kEpsilon);
    EXPECT_NEAR(aabb.max.z, 3.5F, kEpsilon);
}

TEST(WorldAabbFromInstance, IdentityTransformPreservesBounds) {
    const harmonia::Aabb object{.min = {-1.0F, -2.0F, -3.0F}, .max = {4.0F, 5.0F, 6.0F}};
    const harmonia::Xform identity{};
    const auto world = harmonia::worldAabbFromInstance(object, identity);
    EXPECT_NEAR(world.min.x, -1.0F, kEpsilon);
    EXPECT_NEAR(world.min.y, -2.0F, kEpsilon);
    EXPECT_NEAR(world.min.z, -3.0F, kEpsilon);
    EXPECT_NEAR(world.max.x, 4.0F, kEpsilon);
    EXPECT_NEAR(world.max.y, 5.0F, kEpsilon);
    EXPECT_NEAR(world.max.z, 6.0F, kEpsilon);
}

TEST(WorldAabbFromInstance, TranslationShiftsBounds) {
    const harmonia::Aabb object{.min = {-1.0F, -1.0F, -1.0F}, .max = {1.0F, 1.0F, 1.0F}};
    const harmonia::Xform xform{.translation = {10.0F, 20.0F, 30.0F}};
    const auto world = harmonia::worldAabbFromInstance(object, xform);
    EXPECT_NEAR(world.min.x, 9.0F, kEpsilon);
    EXPECT_NEAR(world.max.x, 11.0F, kEpsilon);
    EXPECT_NEAR(world.min.z, 29.0F, kEpsilon);
    EXPECT_NEAR(world.max.z, 31.0F, kEpsilon);
}

TEST(WorldAabbFromInstance, UniformScaleExpandsBounds) {
    const harmonia::Aabb object{.min = {-1.0F, -1.0F, -1.0F}, .max = {1.0F, 1.0F, 1.0F}};
    const harmonia::Xform xform{.scale = {2.0F, 2.0F, 2.0F}};
    const auto world = harmonia::worldAabbFromInstance(object, xform);
    EXPECT_NEAR(world.min.x, -2.0F, kEpsilon);
    EXPECT_NEAR(world.max.x, 2.0F, kEpsilon);
    EXPECT_NEAR(world.min.y, -2.0F, kEpsilon);
    EXPECT_NEAR(world.max.y, 2.0F, kEpsilon);
}

TEST(WorldAabbFromInstance, NonUniformScaleReboundsCorrectly) {
    // Non-uniform scale is the case corner-transform (not half-extent) handling
    // is required for: scaling x by 2 and y by 0.5 must re-bound, not just scale.
    const harmonia::Aabb object{.min = {-1.0F, -1.0F, -1.0F}, .max = {1.0F, 1.0F, 1.0F}};
    const harmonia::Xform xform{.scale = {2.0F, 0.5F, 1.0F}};
    const auto world = harmonia::worldAabbFromInstance(object, xform);
    EXPECT_NEAR(world.min.x, -2.0F, kEpsilon);
    EXPECT_NEAR(world.max.x, 2.0F, kEpsilon);
    EXPECT_NEAR(world.min.y, -0.5F, kEpsilon);
    EXPECT_NEAR(world.max.y, 0.5F, kEpsilon);
}

TEST(WorldAabbFromInstance, RotationSwapsAxes) {
    // 180° rotation about Y negates X and Z (convention-independent). A box from
    // (0,0,0)→(2,1,4) maps to (-2,0,-4)→(0,1,0), proving corners (not half-extents)
    // are transformed and re-bound.
    const harmonia::Aabb object{.min = {0.0F, 0.0F, 0.0F}, .max = {2.0F, 1.0F, 4.0F}};
    const harmonia::Xform xform{.rotation = sm::angleAxis(harmonia::Math::kPi, sm::float3{0.0F, 1.0F, 0.0F})};
    const auto world = harmonia::worldAabbFromInstance(object, xform);
    EXPECT_NEAR(world.min.x, -2.0F, kEpsilon);
    EXPECT_NEAR(world.max.x, 0.0F, kEpsilon);
    EXPECT_NEAR(world.min.y, 0.0F, kEpsilon);
    EXPECT_NEAR(world.max.y, 1.0F, kEpsilon);
    EXPECT_NEAR(world.min.z, -4.0F, kEpsilon);
    EXPECT_NEAR(world.max.z, 0.0F, kEpsilon);
}
