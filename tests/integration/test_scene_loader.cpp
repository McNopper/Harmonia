#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <slang-math/slang-math.hpp>
#include <string>
#include <utility>
#include <vector>

#include "harmonia/scene/SceneLoader.hpp"

namespace {

class RecordingSceneBuilder final : public ISceneBuilder {
  public:
    std::uint32_t addMaterial(Material&& /*mat*/) override {
        ++materialCount;
        return static_cast<std::uint32_t>(materialCount - 1);
    }

    std::uint32_t addTexture(Texture&& /*texture*/) override {
        ++textureCount;
        return static_cast<std::uint32_t>(textureCount - 1);
    }

    std::uint32_t addMesh(const DeviceContext& /*ctx*/,
                          const CommandPool& /*pool*/,
                          MeshData&& data,
                          std::string_view /*name*/) override {
        if (data.vertices.empty() || data.indices.empty()) {
            return std::numeric_limits<std::uint32_t>::max();
        }
        ++meshCount;
        return static_cast<std::uint32_t>(meshCount - 1);
    }

    std::uint32_t addSphereMesh(const DeviceContext& /*ctx*/,
                                const CommandPool& /*pool*/,
                                float radius,
                                std::string_view /*name*/) override {
        if (radius <= 0.0F) {
            return std::numeric_limits<std::uint32_t>::max();
        }
        ++sphereMeshCount;
        return static_cast<std::uint32_t>(meshCount + sphereMeshCount - 1);
    }

    std::uint32_t
    addInstance(std::uint32_t /*meshIndex*/, const Xform& /*xform*/, std::uint32_t /*materialIdx*/) override {
        ++instanceCount;
        return static_cast<std::uint32_t>(instanceCount - 1);
    }

    std::size_t materialCount = 0;
    std::size_t textureCount = 0;
    std::size_t meshCount = 0;
    std::size_t sphereMeshCount = 0;
    std::size_t instanceCount = 0;
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
    // cornell_classic has 1 OBJ (split into N sub-meshes) + 2 boxes; every
    // registered mesh is placed exactly once, so instances == meshes.
    EXPECT_EQ(scene.instanceCount, scene.meshCount);
    EXPECT_GE(scene.instanceCount, 3U);
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

TEST(SceneLoader, ParsesStageTogglesFromRenderPresetAndInlineOverride) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "harmonia_scene_loader_stage_toggles";
    std::filesystem::create_directories(dir / "presets");

    const std::filesystem::path presetPath = dir / "presets" / "render_stage_preset.toml";
    {
        std::ofstream preset(presetPath);
        preset << "enable_accumulation_stage = false\n";
        preset << "enable_denoiser_stage = false\n";
        preset << "enable_tonemapper_stage = true\n";
    }

    const std::filesystem::path scenePath = dir / "stage_toggles.scene.toml";
    {
        std::ofstream out(scenePath);
        out << "[render]\n";
        out << "reference = \"presets/render_stage_preset.toml\"\n";
        out << "enable_denoiser_stage = true\n";
        out << "stages.tonemapper = false\n";
    }

    RecordingSceneBuilder scene;
    const DeviceContext dummyCtx{};
    const CommandPool dummyPool{};

    const auto cfg = SceneLoader::load(scenePath, assetsDir(), scene, dummyCtx, dummyPool);

    ASSERT_TRUE(cfg.has_value());
    ASSERT_TRUE(cfg->accumulationStageEnabled.has_value());
    EXPECT_FALSE(*cfg->accumulationStageEnabled);
    ASSERT_TRUE(cfg->denoiserStageEnabled.has_value());
    EXPECT_TRUE(*cfg->denoiserStageEnabled);
    ASSERT_TRUE(cfg->tonemapperStageEnabled.has_value());
    EXPECT_FALSE(*cfg->tonemapperStageEnabled);

    std::filesystem::remove_all(dir);
}

} // namespace
