#include <cmath>
#include <gtest/gtest.h>
#include <optional>
#include <slang-math/slang-math.hpp>

#include "harmonia/renderer/CameraAssembly.hpp"

namespace {

using harmonia::renderer::Camera;
using harmonia::renderer::CameraAssembly;
using SceneConfig = harmonia::scene::SceneLoader::SceneConfig;

void expectNear(const sm::float3& actual, const sm::float3& expected) {
    EXPECT_NEAR(actual.x, expected.x, 1e-6f);
    EXPECT_NEAR(actual.y, expected.y, 1e-6f);
    EXPECT_NEAR(actual.z, expected.z, 1e-6f);
}

void expectParamsEqual(const Camera::Params& actual, const Camera::Params& expected) {
    expectNear(actual.position, expected.position);
    expectNear(actual.target, expected.target);
    expectNear(actual.up, expected.up);
    EXPECT_FLOAT_EQ(actual.vfovDeg, expected.vfovDeg);
    EXPECT_FLOAT_EQ(actual.aspectRatio, expected.aspectRatio);
    EXPECT_FLOAT_EQ(actual.nearPlane, expected.nearPlane);
    EXPECT_FLOAT_EQ(actual.farPlane, expected.farPlane);
    EXPECT_FLOAT_EQ(actual.lensRadius, expected.lensRadius);
    EXPECT_FLOAT_EQ(actual.focusDist, expected.focusDist);
    EXPECT_FLOAT_EQ(actual.physical.focalLengthMm, expected.physical.focalLengthMm);
    EXPECT_FLOAT_EQ(actual.physical.aperture, expected.physical.aperture);
    EXPECT_FLOAT_EQ(actual.physical.shutterSpeedHz, expected.physical.shutterSpeedHz);
    EXPECT_FLOAT_EQ(actual.physical.iso, expected.physical.iso);
    EXPECT_FLOAT_EQ(actual.physical.sensorSizeMm.x, expected.physical.sensorSizeMm.x);
    EXPECT_FLOAT_EQ(actual.physical.sensorSizeMm.y, expected.physical.sensorSizeMm.y);
}

TEST(CameraAssembly, CrossRendererAssemblyMatchesExactly) {
    SceneConfig config{};
    config.cameraPos = sm::float3{-2.0f, 0.5f, 7.0f};
    config.cameraAt = sm::float3{0.0f, 0.5f, 0.0f};
    config.cameraUp = sm::float3{0.0f, 1.0f, 0.0f};
    config.cameraVfov = 41.0f;

    const auto theiaView = CameraAssembly::fromSceneConfig(config, 1.25f);
    const auto hyperionView = CameraAssembly::fromSceneConfig(config, 1.25f);
    ASSERT_TRUE(theiaView.has_value());
    ASSERT_TRUE(hyperionView.has_value());
    expectParamsEqual(*theiaView, *hyperionView);
}

TEST(CameraAssembly, PrivateFallbackDivergenceIsDetectable) {
    SceneConfig config{};
    config.cameraPos = sm::float3{0.0f, 1.0f, 4.0f};
    config.cameraAt = sm::float3{0.0f, 1.0f, 0.0f};

    const auto shared = CameraAssembly::fromSceneConfig(config, 16.0f / 9.0f);
    ASSERT_TRUE(shared.has_value());

    Camera::Params privateFallback = *shared;
    privateFallback.vfovDeg += 1.0f;
    EXPECT_FALSE(std::abs(privateFallback.vfovDeg - shared->vfovDeg) < 1e-6f);
}

TEST(CameraAssembly, ExplicitSceneFieldsAreAssembledOnce) {
    SceneConfig config{};
    config.cameraPos = sm::float3{1.0f, 2.0f, 3.0f};
    config.cameraAt = sm::float3{4.0f, 6.0f, 3.0f};
    config.cameraUp = sm::float3{0.0f, 0.0f, 1.0f};
    config.cameraVfov = 55.0f;
    config.cameraEv100 = 8.0f;

    const auto params = CameraAssembly::fromSceneConfig(config, 2.0f);
    ASSERT_TRUE(params.has_value());
    expectNear(params->position, *config.cameraPos);
    expectNear(params->target, *config.cameraAt);
    expectNear(params->up, *config.cameraUp);
    EXPECT_FLOAT_EQ(params->vfovDeg, 55.0f);
    EXPECT_FLOAT_EQ(params->aspectRatio, 2.0f);
    EXPECT_FLOAT_EQ(params->physical.ev100(), 8.0f);
    EXPECT_FLOAT_EQ(params->lensRadius, 0.0f);
    EXPECT_FLOAT_EQ(params->focusDist, 5.0f);
    EXPECT_NEAR(params->nearPlane, 0.05f, 1e-6f);
    EXPECT_NEAR(params->farPlane, 5000.0f, 1e-6f);
}

TEST(CameraAssembly, ExposureCompensationIsOneGlobalPowerOfTwo) {
    SceneConfig neutral{};
    neutral.cameraPos = sm::float3{0.0f, 1.0f, 4.0f};
    neutral.cameraAt = sm::float3{0.0f, 1.0f, 0.0f};
    neutral.cameraEv100 = 8.0f;
    SceneConfig compensated = neutral;
    compensated.cameraExposureCompensationEv = 1.5f;
    const auto base = CameraAssembly::fromSceneConfig(neutral, 1.0f);
    const auto adjusted = CameraAssembly::fromSceneConfig(compensated, 1.0f);
    ASSERT_TRUE(base.has_value() && adjusted.has_value());
    EXPECT_FLOAT_EQ(adjusted->physical.ev100(), 6.5f);
    EXPECT_NEAR(adjusted->physical.exposure() / base->physical.exposure(), std::exp2(1.5f), 1.0e-6f);
}

TEST(CameraAssembly, MissingOptionalFieldsUseSharedDefaults) {
    SceneConfig config{};
    config.cameraPos = sm::float3{0.0f, 1.0f, 4.0f};
    config.cameraAt = sm::float3{0.0f, 1.0f, 0.0f};

    const auto params = CameraAssembly::fromSceneConfig(config, 16.0f / 9.0f);
    ASSERT_TRUE(params.has_value());
    expectNear(params->up, sm::float3{0.0f, 1.0f, 0.0f});
    EXPECT_FLOAT_EQ(params->vfovDeg, harmonia::renderer::kDefaultVfovDeg);
    EXPECT_FLOAT_EQ(params->physical.focalLengthMm, 50.0f);
    EXPECT_FLOAT_EQ(params->physical.aperture, 8.0f);
    EXPECT_FLOAT_EQ(params->physical.shutterSpeedHz, 100.0f);
    EXPECT_FLOAT_EQ(params->physical.iso, 100.0f);
}

TEST(CameraAssembly, MissingCameraPlacementIsRejected) {
    SceneConfig missingTarget{};
    missingTarget.cameraPos = sm::float3{0.0f, 1.0f, 4.0f};
    EXPECT_FALSE(CameraAssembly::fromSceneConfig(missingTarget, 1.0f).has_value());

    SceneConfig missingPosition{};
    missingPosition.cameraAt = sm::float3{0.0f, 1.0f, 0.0f};
    EXPECT_FALSE(CameraAssembly::fromSceneConfig(missingPosition, 1.0f).has_value());
}

TEST(CameraAssembly, ThinLensFieldsReachCameraData) {
    SceneConfig config{};
    config.cameraPos = sm::float3{0.0f, 1.0f, 4.0f};
    config.cameraAt = sm::float3{0.0f, 1.0f, 0.0f};
    config.cameraLensRadius = 0.00625f;
    config.cameraFocusDistance = 1.02f;
    const auto params = CameraAssembly::fromSceneConfig(config, 16.0f / 9.0f);
    ASSERT_TRUE(params.has_value());
    EXPECT_FLOAT_EQ(params->lensRadius, 0.00625f);
    EXPECT_FLOAT_EQ(params->focusDist, 1.02f);
    const harmonia::CameraData data = Camera(*params).getCameraData(7U, 8U);
    EXPECT_FLOAT_EQ(data.lensRadius, 0.00625f);
    EXPECT_FLOAT_EQ(data.focusDistance, 1.02f);
}

TEST(CameraAssembly, AnimatedPoseUsesSharedForwardAndUpConvention) {
    Camera::Params base{};
    base.vfovDeg = 47.0f;
    base.aspectRatio = 1.5f;
    base.physical = Camera::PhysicalCamera::fromEv100(6.0f);

    const aether::CameraPose pose{sm::float3{2.0f, 3.0f, 4.0f}, sm::quaternion{0.0f, 0.0f, 0.0f, 1.0f}};
    const Camera::Params placed = CameraAssembly::fromPose(base, pose);

    expectNear(placed.position, pose.position);
    expectNear(placed.target, sm::float3{2.0f, 3.0f, 3.0f});
    expectNear(placed.up, sm::float3{0.0f, 1.0f, 0.0f});
    EXPECT_FLOAT_EQ(placed.vfovDeg, base.vfovDeg);
    EXPECT_FLOAT_EQ(placed.aspectRatio, base.aspectRatio);
    EXPECT_FLOAT_EQ(placed.physical.ev100(), 6.0f);
    EXPECT_NEAR(placed.nearPlane, 0.01f, 1e-6f);
    EXPECT_NEAR(placed.farPlane, 1000.0f, 1e-6f);
}

TEST(CameraAssembly, SharedOutputIsDeterministicAcrossConsumers) {
    SceneConfig config{};
    config.cameraPos = sm::float3{-2.0f, 0.5f, 7.0f};
    config.cameraAt = sm::float3{0.0f, 0.5f, 0.0f};
    config.cameraUp = sm::float3{0.0f, 1.0f, 0.0f};
    config.cameraVfov = 41.0f;

    const auto first = CameraAssembly::fromSceneConfig(config, 1.25f);
    const auto second = CameraAssembly::fromSceneConfig(config, 1.25f);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    expectNear(first->position, second->position);
    expectNear(first->target, second->target);
    expectNear(first->up, second->up);
    EXPECT_EQ(first->vfovDeg, second->vfovDeg);
    EXPECT_EQ(first->aspectRatio, second->aspectRatio);
    EXPECT_EQ(first->nearPlane, second->nearPlane);
    EXPECT_EQ(first->farPlane, second->farPlane);
    EXPECT_EQ(first->physical.ev100(), second->physical.ev100());
}

} // namespace
