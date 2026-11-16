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
// The GPU deformation kernels + renderer integration are the renderer halves
// (pending); this file covers the shared seams they will consume.

#include <volk/volk.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "aether/format/ObjImporter.hpp"
#include "fixtures/VulkanTestFixture.hpp"
#include "harmonia/scene/SceneLoader.hpp"
#include "harmonia/scene_rt/SceneRt.hpp"
#include "harmonia/scene_rt/TriangleMesh.hpp"

namespace {

using harmonia::scene::Geometry;
using harmonia::scene::MeshData;
using harmonia::scene::MeshOpacity;
using harmonia::scene::SceneBase;
using harmonia::scene_rt::TriangleMesh;

/// Minimal concrete SceneBase for the shared seams (the renderers' Scenes live
/// downstream; the substrate must not depend on them).
class StubScene final : public SceneBase {
  public:
    std::uint32_t addSphereMesh(const harmonia::DeviceContext&,
                                const harmonia::core::CommandPool&,
                                float,
                                std::string_view) override {
        return std::numeric_limits<std::uint32_t>::max(); // fixtures never place spheres
    }

  private:
    VkResult buildSceneBuffers(const harmonia::DeviceContext&, const harmonia::core::CommandPool&) override {
        return VK_SUCCESS; // the substrate tests never draw
    }
    std::unique_ptr<Geometry> createTriangleMesh(const harmonia::DeviceContext& ctx,
                                                 const harmonia::core::CommandPool& pool,
                                                 MeshData&& data,
                                                 const MeshOpacity& opacity,
                                                 std::string_view name) override {
        auto mesh = TriangleMesh::create(ctx, pool, std::move(data), opacity, name);
        if (!mesh) {
            return nullptr;
        }
        return std::move(*mesh);
    }
    VkResult finishBuild(const harmonia::DeviceContext& ctx, const harmonia::core::CommandPool& pool) override {
        return harmonia::scene_rt::buildTlas(*this, ctx, pool, m_tlas, m_tlasAddress);
    }

    harmonia::rt::AccelerationStructure m_tlas{};
    VkDeviceAddress m_tlasAddress = 0;
};

// Mini OBJ: a unit quad (2 triangles, full v/vt/vn face refs) - 4 deduplicated
// vertices, the count a [[skin]]'s per-vertex arrays must match.
void writeQuadObj(const std::filesystem::path& path) {
    std::ofstream out(path);
    out << "v 0 0 0\n"
           "v 1 0 0\n"
           "v 1 1 0\n"
           "v 0 1 0\n"
           "vt 0 0\n"
           "vt 1 0\n"
           "vt 1 1\n"
           "vt 0 1\n"
           "vn 0 0 1\n"
           "f 1/1/1 2/2/1 3/3/1\n"
           "f 1/1/1 3/3/1 4/4/1\n";
}

std::size_t quadVertexCount(const std::filesystem::path& path) {
    const auto groups = aether::ObjImporter::parse(path);
    if (!groups) {
        return 0;
    }
    std::size_t total = 0;
    for (const aether::MeshGroup& g : *groups) {
        total += g.mesh.vertices.size();
    }
    return total;
}

/// Skin scene over the quad: one root joint + the meshed part node. When
/// @p animate, an [[animation]] track drives the joint's translation (the
/// poses must then follow the tick clock).
std::string skinSceneToml(std::size_t vertexCount, bool animate) {
    std::ostringstream os;
    if (animate) {
        os << "[animation_clock]\nfps = 30\n\n";
    }
    os << "[[mesh]]\n"
          "name = \"q\"\n"
          "path = \"q.obj\"\n"
          "\n"
          "[[node]]\n"
          "id = \"root\"\n"
          "\n"
          "[[node]]\n"
          "id = \"part\"\n"
          "mesh = \"q\"\n"
          "\n"
          "[[skin]]\n"
          "node = \"part\"\n"
          "joints = [\"root\"]\n"
          "inverse_bind = [[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]]\n"
          "bone_indices = [";
    for (std::size_t v = 0; v < vertexCount; ++v) {
        os << (v == 0 ? "\n  " : ",\n  ") << "[0, 0, 0, 0]";
    }
    os << "\n]\nbone_weights = [";
    for (std::size_t v = 0; v < vertexCount; ++v) {
        os << (v == 0 ? "\n  " : ",\n  ") << "[1, 0, 0, 0]";
    }
    os << "\n]\n";
    if (animate) {
        os << "\n[[animation]]\n"
              "target = \"nodes/root/translation\"\n"
              "interpolation = \"linear\"\n"
              "times = [0.0, 1.0]\n"
              "values = [0, 0, 0, 3, 0, 0]\n";
    }
    return os.str();
}

struct SkinFixture {
    std::filesystem::path dir;
    std::size_t vertexCount = 0;

    SkinFixture(std::string_view name, bool animate) {
        namespace fs = std::filesystem;
        dir = fs::temp_directory_path() / ("harmonia_skin_rt_" + std::string{name});
        fs::create_directories(dir);
        writeQuadObj(dir / "q.obj");
        vertexCount = quadVertexCount(dir / "q.obj");
        std::ofstream scn(dir / "s.scene.toml");
        scn << skinSceneToml(vertexCount, animate);
    }
    ~SkinFixture() { std::filesystem::remove_all(dir); }

    [[nodiscard]] std::filesystem::path scenePath() const { return dir / "s.scene.toml"; }
};

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
    scn << "[[mesh]]\nname = \"q2\"\npath = \"q.obj\"\n\n[[instance]]\nmesh = \"q2\"\n";
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
