#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <slang-math/slang-math.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aether/types/OpacityMicromap.hpp"
#include "harmonia/scene/SceneLoader.hpp"

namespace {

class RecordingSceneBuilder final : public harmonia::scene::ISceneBuilder {
  public:
    std::uint32_t addMaterial(harmonia::scene::Material&& /*mat*/) override {
        ++materialCount;
        return static_cast<std::uint32_t>(materialCount - 1);
    }

    std::uint32_t addTexture(harmonia::scene::Texture&& /*texture*/) override {
        ++textureCount;
        return static_cast<std::uint32_t>(textureCount - 1);
    }

    std::uint32_t addMesh(const harmonia::DeviceContext& /*ctx*/,
                          const harmonia::core::CommandPool& /*pool*/,
                          harmonia::scene::MeshData&& data,
                          const harmonia::scene::MeshOpacity& /*opacity*/,
                          std::string_view /*name*/) override {
        if (data.vertices.empty() || data.indices.empty()) {
            return std::numeric_limits<std::uint32_t>::max();
        }
        ++meshCount;
        return static_cast<std::uint32_t>(meshCount - 1);
    }

    const aether::OpacityMicromapData& addOpacityMicromap(aether::OpacityMicromapData&& data) override {
        ommAssets.push_back(std::move(data));
        return ommAssets.back();
    }

    std::uint32_t addSphereMesh(const harmonia::DeviceContext& /*ctx*/,
                                const harmonia::core::CommandPool& /*pool*/,
                                float radius,
                                std::string_view /*name*/) override {
        if (radius <= 0.0F) {
            return std::numeric_limits<std::uint32_t>::max();
        }
        ++sphereMeshCount;
        return static_cast<std::uint32_t>(meshCount + sphereMeshCount - 1);
    }

    std::uint32_t addInstance(std::uint32_t /*meshIndex*/,
                              const harmonia::scene::Transform& /*transform*/,
                              std::uint32_t /*materialIdx*/) override {
        ++instanceCount;
        return static_cast<std::uint32_t>(instanceCount - 1);
    }

    std::size_t materialCount = 0;
    std::size_t textureCount = 0;
    std::size_t meshCount = 0;
    std::size_t sphereMeshCount = 0;
    std::size_t instanceCount = 0;
    std::vector<aether::OpacityMicromapData> ommAssets;
};

std::filesystem::path assetsDir() {
    return std::filesystem::path{AETHER_ASSETS_DIR};
}

TEST(SceneLoader, LoadsRealSceneAndProducesExpectedConfig) {
    RecordingSceneBuilder scene;
    const harmonia::DeviceContext dummyCtx{};
    const harmonia::core::CommandPool dummyPool{};

    const auto cfg = harmonia::scene::SceneLoader::load(
        assetsDir() / "cornell_classic.scene.toml", assetsDir(), scene, dummyCtx, dummyPool);

    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->workingColorSpace, harmonia::utils::ColorSpace::WorkingColorSpace::LinRec2020);
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

TEST(SceneLoader, UnknownTonemapperStillFallsBackToDefault) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "harmonia_scene_loader_tonemap_fallback";
    std::filesystem::create_directories(dir);
    const std::filesystem::path scenePath = dir / "fallback.scene.toml";
    {
        std::ofstream out(scenePath);
        out << "[tonemap]\n";
        out << "tonemapper = \"unknown_tonemap\"\n";
    }

    RecordingSceneBuilder scene;
    const harmonia::DeviceContext dummyCtx{};
    const harmonia::core::CommandPool dummyPool{};

    const auto cfg = harmonia::scene::SceneLoader::load(scenePath, assetsDir(), scene, dummyCtx, dummyPool);

    ASSERT_TRUE(cfg.has_value());
    EXPECT_FALSE(cfg->tonemapper.has_value());

    std::filesystem::remove_all(dir);
}

// H-05 rejection test: the working color space is always scene-referred linear.
// An unknown token (typo) or a display-referred space (srgb_rec709_scene) must
// abort the scene load — silently defaulting would render the scene in a color
// space the assets were never converted into.
TEST(SceneLoader, UnsupportedWorkingColorSpaceFailsSceneLoad) {
    const std::array rejectedTokens{"lin_unknown_scene", "srgb_rec709_scene"};
    for (const std::string_view token : rejectedTokens) {
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "harmonia_scene_loader_working_space_reject";
        std::filesystem::create_directories(dir);
        const std::filesystem::path scenePath = dir / "reject.scene.toml";
        {
            std::ofstream out(scenePath);
            out << "[render]\n";
            out << "working_color_space = \"" << token << "\"\n";
        }

        RecordingSceneBuilder scene;
        const harmonia::DeviceContext dummyCtx{};
        const harmonia::core::CommandPool dummyPool{};

        const auto cfg = harmonia::scene::SceneLoader::load(scenePath, assetsDir(), scene, dummyCtx, dummyPool);

        ASSERT_FALSE(cfg.has_value()) << "working_color_space = '" << token << "' must fail the scene load";

        std::filesystem::remove_all(dir);
    }
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
    const harmonia::DeviceContext dummyCtx{};
    const harmonia::core::CommandPool dummyPool{};

    const auto cfg = harmonia::scene::SceneLoader::load(scenePath, assetsDir(), scene, dummyCtx, dummyPool);

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
