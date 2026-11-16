// ANI4+ANI5 renderer-half receipt: the GPU deformation kernel (deform.comp.slang)
// matched against BOTH Aether CPU references, at TWO animation times:
//
// (1) skin-only: GPU == deformSkinLbs (job fields, buffer contents verbatim,
//     scene-global range == mesh-local bitwise, a second execute re-stages the
//     matrices - the idempotency receipt that caught the pose-composition bug);
// (2) composed skin+MORPH: GPU == deformMorphs THEN deformSkinLbs (glTF order:
//     object-space morph, then skin the morphed position), with both halves'
//     job fields + the staged weights verified against the evaluated source;
// (3) morph-only: jointCount == 0 path with NULL skin descriptors bound -
//     GPU == deformMorphs directly.
//
// Runs under the fixture's always-on validation layers (the pass's
// transfer/compute/AS-build barriers are what they check).

#include <volk/volk.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <vector>

#include "aether/format/Morphing.hpp"
#include "aether/format/ObjImporter.hpp"
#include "aether/format/SceneAnimator.hpp"
#include "aether/format/Skinning.hpp"
#include "fixtures/SkinTestSupport.hpp"
#include "fixtures/VulkanTestFixture.hpp"
#include "harmonia/GpuTypes.hpp"
#include "harmonia/scene/DeformPass.hpp"
#include "harmonia/scene/SceneLoader.hpp"
#include "harmonia/scene_rt/TriangleMesh.hpp"

namespace {

using harmonia::test::SkinFixture;
using harmonia::test::StubScene;
using harmonia::test::writeQuadObj;

/// Copy [srcOffset, srcOffset+size) of a device buffer into a host-mapped
/// buffer (test_buffer.cpp readback pattern, offset-aware).
[[nodiscard]] std::vector<std::uint8_t> readbackRange(const harmonia::DeviceContext& ctx,
                                                      harmonia::core::CommandPool& pool,
                                                      const harmonia::core::Buffer& src,
                                                      VkDeviceSize srcOffset,
                                                      VkDeviceSize size) {
    auto readback = harmonia::core::Buffer::create(
        ctx, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST, "test.deform.readback");
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

// Quad + a [[morph]] (+ optional [[skin]] + tracks) - generated with the exact
// vertex row counts the bind-time validators require.
struct QuadSceneFixture {
    std::filesystem::path dir;
    std::size_t vertexCount = 0;

    QuadSceneFixture(std::string_view name, std::string_view tomlText) {
        namespace fs = std::filesystem;
        dir = fs::temp_directory_path() / ("harmonia_deform_" + std::string{name});
        fs::create_directories(dir);
        writeQuadObj(dir / "q.obj");
        vertexCount = harmonia::test::quadVertexCount(dir / "q.obj");
        std::ofstream scn(dir / "s.scene.toml");
        scn << tomlText;
    }
    ~QuadSceneFixture() { std::filesystem::remove_all(dir); }

    [[nodiscard]] std::filesystem::path scenePath() const { return dir / "s.scene.toml"; }
};

std::string boneRows(std::size_t vertexCount) {
    std::string out;
    for (std::size_t v = 0; v < vertexCount; ++v) {
        out += (v == 0 ? "\n  " : ",\n  ") + std::string{"[0, 0, 0, 0]"};
    }
    return out;
}

std::string weightRows(std::size_t vertexCount) {
    std::string out;
    for (std::size_t v = 0; v < vertexCount; ++v) {
        out += (v == 0 ? "\n  " : ",\n  ") + std::string{"[1, 0, 0, 0]"};
    }
    return out;
}

/// Composed scene: quad with one joint (translation track 0 -> 2 over t[0,1])
/// AND one morph target (bulge +z on the upper vertices, weight track 0 -> 1).
std::string composedSceneToml(std::size_t vertexCount, bool withSkin) {
    std::ostringstream os;
    os << "[animation_clock]\nfps = 30\n\n"
          "[[mesh]]\n"
          "name = \"q\"\n"
          "path = \"q.obj\"\n"
          "\n"
          "[[node]]\n"
          "id = \"root\"\n"
          "\n"
          "[[node]]\n"
          "id = \"part\"\n"
          "mesh = \"q\"\n";
    if (withSkin) {
        os << "\n[[skin]]\n"
              "node = \"part\"\n"
              "joints = [\"root\"]\n"
              "inverse_bind = [[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]]\n"
              "bone_indices = ["
           << boneRows(vertexCount) << "\n]\nbone_weights = [" << weightRows(vertexCount) << "\n]";
        os << "\n[[animation]]\n"
              "target = \"nodes/root/translation\"\n"
              "interpolation = \"linear\"\n"
              "times = [0.0, 1.0]\n"
              "values = [0, 0, 0, 2, 0, 0]\n";
    }
    // Morph over the quad (rows = vertex count): push the upper row +z.
    os << "\n[[morph]]\n"
          "mesh = \"q\"\n"
          "name = \"bulge\"\n"
          "deltas = [";
    for (std::size_t v = 0; v < vertexCount; ++v) {
        os << (v == 0 ? "\n  " : ",\n  ");
        os << (v >= 2 ? "[0, 0, 1]" : "[0, 0, 0]");
    }
    os << "\n]\n";
    os << "\n[[animation]]\n"
          "target = \"morphs/q/bulge\"\n"
          "interpolation = \"linear\"\n"
          "times = [0.0, 1.0]\n"
          "values = [0.0, 1.0]\n";
    return os.str();
}

} // namespace

// ── (1) skin-only ────────────────────────────────────────────────────────────

TEST_F(VulkanFixture, GpuSkinDeformMatchesCpuReferenceAtTwoTimes) {
    SkinFixture fx("deform", /*animate=*/true);
    ASSERT_GT(fx.vertexCount, 0U);

    StubScene scene;
    const auto cfg = harmonia::scene::SceneLoader::load(fx.scenePath(), fx.dir, scene, deviceCtx(), commandPool());
    ASSERT_TRUE(cfg.has_value());
    ASSERT_EQ(scene.build(deviceCtx(), commandPool()), VK_SUCCESS);

    // Job materialization: one covered mesh, single joint, absolute offsets.
    ASSERT_EQ(scene.deformJobs().size(), 1U);
    EXPECT_EQ(scene.deformJobs()[0].meshIndex, 0U);
    EXPECT_EQ(scene.deformJobs()[0].vertexCount, static_cast<std::uint32_t>(fx.vertexCount));
    EXPECT_EQ(scene.deformJobs()[0].weightRow, 0U);
    EXPECT_EQ(scene.deformJobs()[0].matrixOffset, 0U);
    EXPECT_EQ(scene.deformJobs()[0].jointCount, 1U);
    EXPECT_EQ(scene.deformJobs()[0].morphTargetCount, 0U) << "skin-only job carries no morph fields";

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

    auto pass = harmonia::scene::DeformPass::create(deviceCtx(),
                                                    scene,
                                                    scene.vertexRestBuffer(),
                                                    scene.sceneVertices(),
                                                    std::filesystem::path(HARMONIA_SHADER_DIR) / "deform.comp.spv");
    ASSERT_TRUE(pass.has_value()) << static_cast<int>(pass.error());

    // The GPU buffers must hold the source skin rows verbatim (a swapped or
    // zeroed upload silently early-returns the kernel to the rest pose).
    const VkDeviceSize rowBytes = fx.vertexCount * 16;
    const auto idxBytes = readbackRange(deviceCtx(), commandPool(), scene.skinIndexBuffer(), 0, rowBytes);
    ASSERT_EQ(idxBytes.size(), static_cast<std::size_t>(rowBytes));
    EXPECT_EQ(*reinterpret_cast<const std::uint32_t*>(idxBytes.data()), 0U);
    const auto weightBytes = readbackRange(deviceCtx(), commandPool(), scene.skinWeightBuffer(), 0, rowBytes);
    ASSERT_EQ(weightBytes.size(), static_cast<std::size_t>(rowBytes));
    EXPECT_FLOAT_EQ(*reinterpret_cast<const float*>(weightBytes.data()), 1.0F);

    const VkDeviceSize vertexBytes = fx.vertexCount * sizeof(harmonia::GpuVertex);
    ASSERT_EQ(scene.meshSceneVertexOffsets().size(), 1U);
    const VkDeviceSize globalOffset =
        static_cast<VkDeviceSize>(scene.meshSceneVertexOffsets()[0]) * sizeof(harmonia::GpuVertex);
    ASSERT_LE(globalOffset + vertexBytes, scene.sceneVertices().size());

    for (const float t : {0.5F, 1.0F}) {
        const auto poses = aether::SceneAnimator::evaluateSkins(*source, t);
        ASSERT_EQ(poses.size(), 1U);
        const auto cpu = aether::deformSkinLbs(rest, source->skins[0], poses[0].skinMatrices);
        ASSERT_TRUE(cpu.has_value());

        ASSERT_EQ(pass->execute(deviceCtx(),
                                commandPool(),
                                scene,
                                t,
                                scene.vertexRestBuffer(),
                                scene.sceneVertices(),
                                scene.meshSceneVertexOffsets()),
                  VK_SUCCESS)
            << "t=" << t;

        const auto local = readbackRange(deviceCtx(), commandPool(), mesh->vertexBuffer(), 0, vertexBytes);
        ASSERT_EQ(local.size(), static_cast<std::size_t>(vertexBytes)) << "t=" << t;
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
        if (t > 0.0F) {
            const float expectedX = 3.0F * t;
            EXPECT_NEAR(cpu->positions[0].x, rest.vertices[0].position.x + expectedX, kEps);
            EXPECT_NEAR(cpu->normals[0].z, 1.0F, kEps);
        }
    }
}

// ── (2) composed skin + morph (glTF order) ───────────────────────────────────

TEST_F(VulkanFixture, GpuMorphThenSkinMatchesCpuCompositionAtTwoTimes) {
    QuadSceneFixture fx("composed", composedSceneToml(0, /*withSkin=*/true)); // rows patched below
    ASSERT_GT(fx.vertexCount, 0U);
    // Regenerate with the exact vertex count (the fixture wrote rows for 0).
    {
        std::ofstream scn(fx.scenePath());
        scn << composedSceneToml(fx.vertexCount, true);
    }

    StubScene scene;
    const auto cfg = harmonia::scene::SceneLoader::load(fx.scenePath(), fx.dir, scene, deviceCtx(), commandPool());
    ASSERT_TRUE(cfg.has_value());
    ASSERT_EQ(scene.build(deviceCtx(), commandPool()), VK_SUCCESS);

    // ONE merged job carrying both halves (skin AND morph on the same mesh).
    ASSERT_EQ(scene.deformJobs().size(), 1U);
    const harmonia::scene::DeformJob& job = scene.deformJobs()[0];
    EXPECT_EQ(job.vertexCount, static_cast<std::uint32_t>(fx.vertexCount));
    EXPECT_EQ(job.jointCount, 1U) << "skin half bound";
    EXPECT_EQ(job.morphTargetCount, 1U) << "morph half bound";
    EXPECT_EQ(job.morphDeltaStride, static_cast<std::uint32_t>(fx.vertexCount));
    EXPECT_EQ(job.morphDeltaOffset, 0U);
    EXPECT_EQ(job.morphWeightOffset, 0U);
    EXPECT_TRUE(scene.meshes()[0]->isDynamic());

    const aether::SceneDesc* source = scene.attachedScene();
    ASSERT_NE(source, nullptr);
    ASSERT_EQ(source->morphs.size(), 1U);
    ASSERT_EQ(source->skins.size(), 1U);

    const auto groups = aether::ObjImporter::parse(fx.dir / "q.obj");
    ASSERT_TRUE(groups.has_value());
    const aether::MeshData& rest = (*groups)[0].mesh;

    // Static buffer receipts: deltas are the authored rows pad-w; the staged
    // weights follow the evaluated morph weights.
    const auto deltaBytes = readbackRange(deviceCtx(), commandPool(), scene.morphDeltaBuffer(), 0, fx.vertexCount * 16);
    ASSERT_EQ(deltaBytes.size(), fx.vertexCount * 16);
    const auto* delta0 = reinterpret_cast<const float*>(deltaBytes.data());
    EXPECT_FLOAT_EQ(delta0[0], 0.0F);
    EXPECT_FLOAT_EQ(delta0[1], 0.0F);
    EXPECT_FLOAT_EQ(delta0[2], 0.0F);
    const auto* deltaLast = reinterpret_cast<const float*>(deltaBytes.data() + (fx.vertexCount - 1) * 16);
    EXPECT_FLOAT_EQ(deltaLast[2], 1.0F) << "the upper-row +z bulge must be uploaded";

    const auto* mesh = dynamic_cast<const harmonia::scene_rt::TriangleMesh*>(scene.meshes()[0].get());
    ASSERT_NE(mesh, nullptr);

    auto pass = harmonia::scene::DeformPass::create(deviceCtx(),
                                                    scene,
                                                    scene.vertexRestBuffer(),
                                                    scene.sceneVertices(),
                                                    std::filesystem::path(HARMONIA_SHADER_DIR) / "deform.comp.spv");
    ASSERT_TRUE(pass.has_value()) << static_cast<int>(pass.error());

    const VkDeviceSize vertexBytes = fx.vertexCount * sizeof(harmonia::GpuVertex);

    for (const float t : {0.5F, 1.0F}) {
        // CPU reference in glTF order: morph the rest pose, then skin the
        // morphed positions with the evaluated pose.
        const auto poses = aether::SceneAnimator::evaluateSkins(*source, t);
        ASSERT_EQ(poses.size(), 1U);
        const auto weights = aether::SceneAnimator::evaluateMorphWeights(*source, t);
        ASSERT_EQ(weights.size(), 1U);
        const auto morphed = aether::deformMorphs(rest, source->morphs, weights);
        ASSERT_TRUE(morphed.has_value());
        aether::MeshData morphedMesh = rest;
        for (std::size_t v = 0; v < rest.vertices.size(); ++v) {
            morphedMesh.vertices[v].position = morphed->positions[v];
        }
        const auto cpu = aether::deformSkinLbs(morphedMesh, source->skins[0], poses[0].skinMatrices);
        ASSERT_TRUE(cpu.has_value());

        ASSERT_EQ(pass->execute(deviceCtx(),
                                commandPool(),
                                scene,
                                t,
                                scene.vertexRestBuffer(),
                                scene.sceneVertices(),
                                scene.meshSceneVertexOffsets()),
                  VK_SUCCESS)
            << "t=" << t;

        // The staged weights track the evaluation (0 at t=0, 1 at t=1).
        const auto stagedWeights =
            readbackRange(deviceCtx(), commandPool(), scene.morphWeightBuffer(), 0, sizeof(float));
        ASSERT_EQ(stagedWeights.size(), sizeof(float));
        EXPECT_NEAR(*reinterpret_cast<const float*>(stagedWeights.data()), t, kEps) << "t=" << t;

        const auto local = readbackRange(deviceCtx(), commandPool(), mesh->vertexBuffer(), 0, vertexBytes);
        ASSERT_EQ(local.size(), static_cast<std::size_t>(vertexBytes)) << "t=" << t;

        for (std::size_t v = 0; v < fx.vertexCount; ++v) {
            const auto* gpu =
                reinterpret_cast<const harmonia::GpuVertex*>(local.data() + v * sizeof(harmonia::GpuVertex));
            EXPECT_NEAR(gpu->position.x, cpu->positions[v].x, kEps) << "t=" << t << " v=" << v;
            EXPECT_NEAR(gpu->position.y, cpu->positions[v].y, kEps) << "t=" << t << " v=" << v;
            EXPECT_NEAR(gpu->position.z, cpu->positions[v].z, kEps) << "t=" << t << " v=" << v;
            EXPECT_NEAR(gpu->normal.x, cpu->normals[v].x, kEps) << "t=" << t << " v=" << v;
            EXPECT_NEAR(gpu->normal.z, cpu->normals[v].z, kEps) << "t=" << t << " v=" << v;
        }
        // The composition must actually differ from pure skinning (the morph
        // half contributes): the upper row (zero skin blend weight change aside)
        // gets pushed +z by weight*1.
        if (t > 0.0F) {
            const std::size_t upper = fx.vertexCount - 1; // delta [0, 0, 1] row
            const auto* gpuUpper =
                reinterpret_cast<const harmonia::GpuVertex*>(local.data() + upper * sizeof(harmonia::GpuVertex));
            EXPECT_GT(gpuUpper->position.z, 0.25F * t - kEps) << "bulge must move the upper row +z at t=" << t;
        }
    }
}

// ── (3) morph-only (NULL skin descriptors) ───────────────────────────────────

TEST_F(VulkanFixture, GpuMorphOnlyMatchesCpuWithoutSkinBuffers) {
    QuadSceneFixture fx("morphonly", composedSceneToml(0, /*withSkin=*/false));
    ASSERT_GT(fx.vertexCount, 0U);
    {
        std::ofstream scn(fx.scenePath());
        scn << composedSceneToml(fx.vertexCount, false);
    }

    StubScene scene;
    const auto cfg = harmonia::scene::SceneLoader::load(fx.scenePath(), fx.dir, scene, deviceCtx(), commandPool());
    ASSERT_TRUE(cfg.has_value());
    ASSERT_TRUE(scene.hasMorphs());
    EXPECT_FALSE(scene.hasSkins());
    ASSERT_EQ(scene.build(deviceCtx(), commandPool()), VK_SUCCESS);

    ASSERT_EQ(scene.deformJobs().size(), 1U);
    EXPECT_EQ(scene.deformJobs()[0].jointCount, 0U) << "morph-only job carries no skin fields";
    EXPECT_EQ(scene.deformJobs()[0].morphTargetCount, 1U);
    EXPECT_FALSE(scene.skinIndexBuffer().isValid()) << "skin buffers stay unallocated on morph-only scenes";
    EXPECT_TRUE(scene.meshes()[0]->isDynamic());

    const aether::SceneDesc* source = scene.attachedScene();
    ASSERT_NE(source, nullptr);
    const auto groups = aether::ObjImporter::parse(fx.dir / "q.obj");
    ASSERT_TRUE(groups.has_value());
    const aether::MeshData& rest = (*groups)[0].mesh;

    auto pass = harmonia::scene::DeformPass::create(deviceCtx(),
                                                    scene,
                                                    scene.vertexRestBuffer(),
                                                    scene.sceneVertices(),
                                                    std::filesystem::path(HARMONIA_SHADER_DIR) / "deform.comp.spv");
    ASSERT_TRUE(pass.has_value()) << static_cast<int>(pass.error());

    const auto* mesh = dynamic_cast<const harmonia::scene_rt::TriangleMesh*>(scene.meshes()[0].get());
    ASSERT_NE(mesh, nullptr);
    const VkDeviceSize vertexBytes = fx.vertexCount * sizeof(harmonia::GpuVertex);

    for (const float t : {0.5F, 1.0F}) {
        const auto weights = aether::SceneAnimator::evaluateMorphWeights(*source, t);
        ASSERT_EQ(weights.size(), 1U);
        const auto cpu = aether::deformMorphs(rest, source->morphs, weights);
        ASSERT_TRUE(cpu.has_value());

        ASSERT_EQ(pass->execute(deviceCtx(),
                                commandPool(),
                                scene,
                                t,
                                scene.vertexRestBuffer(),
                                scene.sceneVertices(),
                                scene.meshSceneVertexOffsets()),
                  VK_SUCCESS)
            << "t=" << t;

        const auto local = readbackRange(deviceCtx(), commandPool(), mesh->vertexBuffer(), 0, vertexBytes);
        ASSERT_EQ(local.size(), static_cast<std::size_t>(vertexBytes)) << "t=" << t;
        for (std::size_t v = 0; v < fx.vertexCount; ++v) {
            const auto* gpu =
                reinterpret_cast<const harmonia::GpuVertex*>(local.data() + v * sizeof(harmonia::GpuVertex));
            EXPECT_NEAR(gpu->position.x, cpu->positions[v].x, kEps) << "t=" << t << " v=" << v;
            EXPECT_NEAR(gpu->position.y, cpu->positions[v].y, kEps) << "t=" << t << " v=" << v;
            EXPECT_NEAR(gpu->position.z, cpu->positions[v].z, kEps) << "t=" << t << " v=" << v;
            // Morphs never touch normals (documented v1 contract).
            EXPECT_NEAR(gpu->normal.z, rest.vertices[v].normal.z, kEps) << "t=" << t << " v=" << v;
        }
    }
}
