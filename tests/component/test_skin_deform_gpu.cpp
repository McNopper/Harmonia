// ANI4 renderer-half receipt: the GPU skinning kernel (skin_deform.comp.slang)
// matched against Aether's deformSkinLbs CPU reference at TWO animation times:
//
// (1) job materialization (loader within-skin row -> absolute job fields);
// (2) the mesh-local vertex buffer (the BLAS refit input) equals the CPU
//     reference positions/normals for the evaluated pose;
// (3) the scene-global vertex range (what every shading stage reads) equals
//     the mesh-local bytes bitwise - the vkCmdCopyBuffer refresh cannot drift;
// (4) a second execute at a different time re-stages the matrices
//     (vkCmdUpdateBuffer path) and the output follows the new pose.
//
// Runs under the fixture's always-on validation layers (the pass's
// transfer/compute/AS-build barriers are what they check).

#include <volk/volk.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "aether/format/ObjImporter.hpp"
#include "aether/format/SceneAnimator.hpp"
#include "aether/format/Skinning.hpp"
#include "fixtures/SkinTestSupport.hpp"
#include "fixtures/VulkanTestFixture.hpp"
#include "harmonia/GpuTypes.hpp"
#include "harmonia/scene/SceneLoader.hpp"
#include "harmonia/scene/SkinDeformPass.hpp"

namespace {

using harmonia::test::SkinFixture;
using harmonia::test::StubScene;

/// Copy [srcOffset, srcOffset+size) of a device buffer into a host-mapped
/// buffer (test_buffer.cpp readback pattern, offset-aware).
[[nodiscard]] std::vector<std::uint8_t> readbackRange(const harmonia::DeviceContext& ctx,
                                                      harmonia::core::CommandPool& pool,
                                                      const harmonia::core::Buffer& src,
                                                      VkDeviceSize srcOffset,
                                                      VkDeviceSize size) {
    auto readback = harmonia::core::Buffer::create(
        ctx, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST, "test.skin.readback");
    if (!readback || readback->mappedData() == nullptr) {
        return {};
    }
    auto cmd = pool.beginOneShot();
    if (!cmd) {
        return {};
    }
    const VkBufferCopy region{.srcOffset = srcOffset, .dstOffset = 0, .size = size};
    vkCmdCopyBuffer(*cmd, src.handle(), readback->handle(), 1, &region);
    if (pool.endOneShot(*cmd) != VK_SUCCESS) {
        return {};
    }
    std::vector<std::uint8_t> result(static_cast<std::size_t>(size));
    std::memcpy(result.data(), readback->mappedData(), result.size());
    return result;
}

constexpr float kEps = 1.0e-4F;

} // namespace

TEST_F(VulkanFixture, GpuSkinDeformMatchesCpuReferenceAtTwoTimes) {
    SkinFixture fx("deform", /*animate=*/true);
    ASSERT_GT(fx.vertexCount, 0U);

    StubScene scene;
    const auto cfg = harmonia::scene::SceneLoader::load(fx.scenePath(), fx.dir, scene, deviceCtx(), commandPool());
    ASSERT_TRUE(cfg.has_value());
    ASSERT_EQ(scene.build(deviceCtx(), commandPool()), VK_SUCCESS);

    // (1) job materialization: one covered mesh, single joint, absolute offsets.
    ASSERT_EQ(scene.skinJobs().size(), 1U);
    EXPECT_EQ(scene.skinJobs()[0].meshIndex, 0U);
    EXPECT_EQ(scene.skinJobs()[0].vertexCount, static_cast<std::uint32_t>(fx.vertexCount));
    EXPECT_EQ(scene.skinJobs()[0].weightRow, 0U);
    EXPECT_EQ(scene.skinJobs()[0].matrixOffset, 0U);
    EXPECT_EQ(scene.skinJobs()[0].jointCount, 1U);

    // CPU-reference inputs: the rest-pose mesh (fixture OBJ) + the attached
    // source the scene actually rendered from.
    const auto groups = aether::ObjImporter::parse(fx.dir / "q.obj");
    ASSERT_TRUE(groups.has_value());
    ASSERT_EQ(groups->size(), 1U);
    const aether::MeshData& rest = (*groups)[0].mesh;
    ASSERT_EQ(rest.vertices.size(), fx.vertexCount);

    const aether::SceneDesc* source = scene.attachedScene();
    ASSERT_NE(source, nullptr);
    ASSERT_EQ(source->skins.size(), 1U);

    const auto* mesh = dynamic_cast<const harmonia::scene_rt::TriangleMesh*>(scene.meshes()[0].get());
    ASSERT_NE(mesh, nullptr);
    ASSERT_NE(mesh->storageVertexBuffer(), nullptr);

    auto pass =
        harmonia::scene::SkinDeformPass::create(deviceCtx(),
                                                scene,
                                                scene.vertexRestBuffer(),
                                                scene.sceneVertices(),
                                                std::filesystem::path(HARMONIA_SHADER_DIR) / "skin_deform.comp.spv");
    ASSERT_TRUE(pass.has_value()) << static_cast<int>(pass.error());

    // The GPU buffers must hold the source skin rows verbatim (a swapped or
    // zeroed upload silently early-returns the kernel to the rest pose).
    const VkDeviceSize rowBytes = fx.vertexCount * 16; // 4 entries x 4 bytes per row
    const auto idxBytes = readbackRange(deviceCtx(), commandPool(), scene.skinIndexBuffer(), 0, rowBytes);
    ASSERT_EQ(idxBytes.size(), static_cast<std::size_t>(rowBytes));
    const auto* firstIdx = reinterpret_cast<const std::uint32_t*>(idxBytes.data());
    EXPECT_EQ(firstIdx[0], 0U) << "joint index row 0 slot 0 must be 0";
    const auto weightBytes = readbackRange(deviceCtx(), commandPool(), scene.skinWeightBuffer(), 0, rowBytes);
    ASSERT_EQ(weightBytes.size(), static_cast<std::size_t>(rowBytes));
    const auto* firstWeight = reinterpret_cast<const float*>(weightBytes.data());
    EXPECT_FLOAT_EQ(firstWeight[0], 1.0F) << "weight row 0 slot 0 must be 1";
    EXPECT_FLOAT_EQ(firstWeight[1], 0.0F);

    const VkDeviceSize vertexBytes = fx.vertexCount * sizeof(harmonia::GpuVertex);
    ASSERT_EQ(scene.meshSceneVertexOffsets().size(), 1U);
    const VkDeviceSize globalOffset =
        static_cast<VkDeviceSize>(scene.meshSceneVertexOffsets()[0]) * sizeof(harmonia::GpuVertex);
    ASSERT_LE(globalOffset + vertexBytes, scene.sceneVertices().size());

    for (const float t : {0.5F, 1.0F}) {
        // Track: root x 0 -> 3 over t [0, 1]; t=0.5 stages translation x=1.5,
        // t=1.0 stages x=3 - so a stale/zero matrix buffer fails loudly.
        const auto poses = aether::SceneAnimator::evaluateSkins(*source, t);
        ASSERT_EQ(poses.size(), 1U);
        ASSERT_EQ(poses[0].skinMatrices.size(), 1U);
        const auto cpu = aether::deformSkinLbs(rest, source->skins[0], poses[0].skinMatrices);
        ASSERT_TRUE(cpu.has_value());
        ASSERT_EQ(cpu->positions.size(), fx.vertexCount);

        ASSERT_EQ(pass->execute(deviceCtx(),
                                commandPool(),
                                scene,
                                t,
                                scene.vertexRestBuffer(),
                                scene.sceneVertices(),
                                scene.meshSceneVertexOffsets()),
                  VK_SUCCESS)
            << "t=" << t;

        // (2) mesh-local (BLAS input) == CPU reference.
        const auto local = readbackRange(deviceCtx(), commandPool(), mesh->vertexBuffer(), 0, vertexBytes);
        ASSERT_EQ(local.size(), static_cast<std::size_t>(vertexBytes)) << "t=" << t;
        // (3) scene-global range == mesh-local, bitwise.
        const auto global = readbackRange(deviceCtx(), commandPool(), scene.sceneVertices(), globalOffset, vertexBytes);
        ASSERT_EQ(global.size(), static_cast<std::size_t>(vertexBytes)) << "t=" << t;
        EXPECT_EQ(global, local) << "scene-global range drifted from the mesh-local copy at t=" << t;

        for (std::size_t v = 0; v < fx.vertexCount; ++v) {
            const auto* gpu =
                reinterpret_cast<const harmonia::GpuVertex*>(local.data() + v * sizeof(harmonia::GpuVertex));
            EXPECT_NEAR(gpu->position.x, cpu->positions[v].x, kEps) << "t=" << t << " v=" << v;
            EXPECT_NEAR(gpu->position.y, cpu->positions[v].y, kEps) << "t=" << t << " v=" << v;
            EXPECT_NEAR(gpu->position.z, cpu->positions[v].z, kEps) << "t=" << t << " v=" << v;
            EXPECT_NEAR(gpu->normal.x, cpu->normals[v].x, kEps) << "t=" << t << " v=" << v;
            EXPECT_NEAR(gpu->normal.y, cpu->normals[v].y, kEps) << "t=" << t << " v=" << v;
            EXPECT_NEAR(gpu->normal.z, cpu->normals[v].z, kEps) << "t=" << t << " v=" << v;
        }
        // (4) the pose actually moved (translation must leak into positions,
        // never into normals - homogeneous w gating).
        if (t > 0.0F) {
            const float expectedX = 3.0F * t; // joint translation x at time t
            EXPECT_NEAR(cpu->positions[0].x, rest.vertices[0].position.x + expectedX, kEps);
            EXPECT_NEAR(cpu->normals[0].z, 1.0F, kEps);
        }
    }
}
