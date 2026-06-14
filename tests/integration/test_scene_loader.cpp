#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "harmonia/scene/SceneLoader.hpp"

namespace {

class RecordingSceneBuilder final : public ISceneBuilder {
  public:
    uint32_t addMaterial(Material /*mat*/) override {
        ++materialCount;
        return static_cast<uint32_t>(materialCount - 1);
    }

    uint32_t addTexture(Texture /*texture*/) override {
        ++textureCount;
        return static_cast<uint32_t>(textureCount - 1);
    }

    uint32_t addMesh(const DeviceContext& /*ctx*/,
                     const CommandPool& /*pool*/,
                     MeshData data,
                     uint32_t /*materialIdx*/,
                     std::string_view /*name*/) override {
        if (data.vertices.empty() || data.indices.empty()) {
            return std::numeric_limits<uint32_t>::max();
        }
        ++meshCount;
        return static_cast<uint32_t>(meshCount - 1);
    }

    uint32_t addSphere(const DeviceContext& /*ctx*/,
                       const CommandPool& /*pool*/,
                       glm::vec3 /*center*/,
                       float radius,
                       uint32_t /*materialIdx*/) override {
        if (radius <= 0.0F) {
            return std::numeric_limits<uint32_t>::max();
        }
        ++sphereCount;
        return static_cast<uint32_t>(sphereCount - 1);
    }

    size_t materialCount = 0;
    size_t textureCount = 0;
    size_t meshCount = 0;
    size_t sphereCount = 0;
};

std::filesystem::path assetsDir() {
    return std::filesystem::path{AETHER_ASSETS_DIR};
}

TEST(SceneLoader, LoadsRealSceneAndProducesExpectedConfig) {
    RecordingSceneBuilder scene;
    const DeviceContext dummyCtx{};
    const CommandPool dummyPool{};

    const auto cfg =
        SceneLoader::load(assetsDir() / "cornell_classic.scene.toml", assetsDir(), scene, dummyCtx, dummyPool);

    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->workingColorSpace, ColorSpace::WorkingColorSpace::LinRec2020);
    ASSERT_TRUE(cfg->spp.has_value());
    EXPECT_EQ(*cfg->spp, 64U);
    ASSERT_TRUE(cfg->maxDepth.has_value());
    EXPECT_EQ(*cfg->maxDepth, 8U);
    ASSERT_TRUE(cfg->cameraVfov.has_value());
    EXPECT_FLOAT_EQ(*cfg->cameraVfov, 39.1F);
    ASSERT_TRUE(cfg->cameraEv100.has_value());
    EXPECT_FLOAT_EQ(*cfg->cameraEv100, 7.0F);
    EXPECT_GT(scene.meshCount, 0U);
}

TEST(SceneLoader, UnknownTonemapperAndWorkingSpaceFallbackToDefaults) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "harmonia_scene_loader_fallback";
    std::filesystem::create_directories(dir);
    const std::filesystem::path scenePath = dir / "fallback.scene.toml";
    {
        std::ofstream out(scenePath);
        out << "[render]\n";
        out << "working_color_space = \"lin_unknown_scene\"\n";
        out << "samples_per_pixel = 4\n";
        out << "max_depth = 2\n\n";
        out << "[tonemap]\n";
        out << "tonemapper = \"unknown_tonemap\"\n";
    }

    RecordingSceneBuilder scene;
    const DeviceContext dummyCtx{};
    const CommandPool dummyPool{};

    const auto cfg = SceneLoader::load(scenePath, assetsDir(), scene, dummyCtx, dummyPool);

    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->workingColorSpace, ColorSpace::WorkingColorSpace::LinRec2020);
    EXPECT_FALSE(cfg->tonemapper.has_value());
    ASSERT_TRUE(cfg->spp.has_value());
    EXPECT_EQ(*cfg->spp, 4U);
    ASSERT_TRUE(cfg->maxDepth.has_value());
    EXPECT_EQ(*cfg->maxDepth, 2U);

    std::filesystem::remove_all(dir);
}

} // namespace
