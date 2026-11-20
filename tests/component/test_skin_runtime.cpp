// ANI4 substrate receipts (device fixture):
//
// (1) The loader attaches a skin-only source (no [[animation]] tracks) and
//     SceneBase::evaluateSkins returns the rest-pose matrices; with an
//     animated joint the poses follow the track.
// (2) A covered mesh builds its BLAS with ALLOW_UPDATE (isDynamic reaches the
//     geometry) and refitDynamicBlases refits it in place - handle stable,
//     validation-clean under the fixture's always-on layers.
// (3) A skin-free scene's refit is a success no-op.
//
// The GPU deformation kernel receipt (GPU vs the deformMorphs +
// deformSkinLbs CPU references) lives in test_deform_gpu.cpp; this file covers
// the shared seams.

#include <volk/volk.h>

#include <cstddef>
#include <fstream>
#include <gtest/gtest.h>
#include <string_view>

#include "fixtures/SkinTestSupport.hpp"
#include "fixtures/VulkanTestFixture.hpp"
#include "harmonia/scene/SceneLoader.hpp"

namespace {

using harmonia::test::SkinFixture;
using harmonia::test::StubScene;

} // namespace

TEST_F(VulkanFixture, SkinOnlySourceAttachesAndPosesAreRestPose) {
    SkinFixture fx("poses", /*animate=*/false);
    ASSERT_GT(fx.vertexCount, 0U);

    StubScene scene;
    const auto cfg = harmonia::scene::SceneLoader::load(fx.scenePath(), fx.dir, scene, deviceCtx(), commandPool());
    ASSERT_TRUE(cfg.has_value());
    EXPECT_TRUE(scene.hasSkins());
    EXPECT_FALSE(scene.hasAnimation()) << "a skin-only scene attaches without [[animation]] tracks";

    const auto poses = scene.evaluateSkins(123.0F);
    ASSERT_EQ(poses.size(), 1U);
    EXPECT_EQ(poses[0].nodeId, "part");
    ASSERT_EQ(poses[0].skinMatrices.size(), 1U);
    // rest pose: root world = identity, IBM = identity -> identity skin matrix.
    EXPECT_NEAR(poses[0].skinMatrices[0][0][0], 1.0F, 1.0e-5F);
    EXPECT_NEAR(poses[0].skinMatrices[0][1][1], 1.0F, 1.0e-5F);
    EXPECT_NEAR(poses[0].skinMatrices[0][0][3], 0.0F, 1.0e-5F);
}

TEST_F(VulkanFixture, AnimatedJointDrivesThePoses) {
    SkinFixture fx("animated", /*animate=*/true);
    ASSERT_GT(fx.vertexCount, 0U);

    StubScene scene;
    const auto cfg = harmonia::scene::SceneLoader::load(fx.scenePath(), fx.dir, scene, deviceCtx(), commandPool());
    ASSERT_TRUE(cfg.has_value());
    ASSERT_TRUE(scene.hasSkins());
    ASSERT_TRUE(scene.hasAnimation());

    const auto pose0 = scene.evaluateSkins(0.0F);
    const auto pose1 = scene.evaluateSkins(1.0F); // track: root x 0 -> 3 over [0, 1]
    ASSERT_EQ(pose0.size(), 1U);
    ASSERT_EQ(pose1.size(), 1U);
    ASSERT_EQ(pose0[0].skinMatrices.size(), 1U);
    EXPECT_NEAR(pose0[0].skinMatrices[0][0][3], 0.0F, 1.0e-4F) << "t=0: joint at rest";
    EXPECT_NEAR(pose1[0].skinMatrices[0][0][3], 3.0F, 1.0e-4F) << "t=1: joint translated to x=3";
}

TEST_F(VulkanFixture, DynamicMeshBuildsUpdateCapableAndRefitsInPlace) {
    SkinFixture fx("refit", /*animate=*/false);
    ASSERT_GT(fx.vertexCount, 0U);

    StubScene scene;
    const auto cfg = harmonia::scene::SceneLoader::load(fx.scenePath(), fx.dir, scene, deviceCtx(), commandPool());
    ASSERT_TRUE(cfg.has_value());
    ASSERT_EQ(scene.build(deviceCtx(), commandPool()), VK_SUCCESS);
    ASSERT_EQ(scene.meshes().size(), 1U);
    ASSERT_TRUE(scene.meshes()[0]->isDynamic()) << "the loader must mark the covered mesh dynamic";

    const VkAccelerationStructureKHR handleBefore = scene.meshes()[0]->blas();
    ASSERT_NE(handleBefore, VK_NULL_HANDLE);

    // Same-vertex refit: legal no-op geometry-wise; it exercises the UPDATE path
    // (ALLOW_UPDATE must have been set at build - validation would flag it otherwise).
    ASSERT_EQ(scene.refitDynamicBlases(deviceCtx(), commandPool()), VK_SUCCESS);
    EXPECT_EQ(scene.meshes()[0]->blas(), handleBefore) << "refit keeps the handle";
}

TEST_F(VulkanFixture, SkinFreeSceneRefitIsSuccessNoop) {
    SkinFixture fx("static", /*animate=*/false);
    // Second mesh WITHOUT a skin: load a skin-free scene by writing one.
    std::ofstream scn(fx.dir / "plain.scene.toml");
    scn << "[[mesh]]\nname = \"q2\"\npath = \"q.obj\"\n\n[[instance]]\nmesh = \"q2\"\n"
           "\n[camera]\ntranslate = [0.0, 1.0, 4.0]\nrotate_x = -10.0\nrotate_y = 0.0\n";
    scn.close();

    StubScene scene;
    const auto cfg =
        harmonia::scene::SceneLoader::load(fx.dir / "plain.scene.toml", fx.dir, scene, deviceCtx(), commandPool());
    ASSERT_TRUE(cfg.has_value());
    EXPECT_FALSE(scene.hasSkins());
    ASSERT_EQ(scene.build(deviceCtx(), commandPool()), VK_SUCCESS);
    ASSERT_EQ(scene.meshes().size(), 1U);
    EXPECT_FALSE(scene.meshes()[0]->isDynamic());
    EXPECT_EQ(scene.refitDynamicBlases(deviceCtx(), commandPool()), VK_SUCCESS);
}
