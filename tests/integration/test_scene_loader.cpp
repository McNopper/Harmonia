#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <slang-math/slang-math.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aether/format/ObjImporter.hpp"
#include "aether/types/OpacityMicromap.hpp"
#include "harmonia/scene/SceneLoader.hpp"

namespace {

class RecordingSceneBuilder final : public harmonia::scene::ISceneBuilder {
  public:
    std::uint32_t addMaterial(harmonia::scene::Material&& mat) override {
        const auto& c = mat.gpu().baseColorWeight;
        materialColors.emplace_back(c.x, c.y, c.z); // oracle for the material-resolution tests
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

    std::uint32_t addInstance(std::uint32_t meshIndex,
                              const harmonia::scene::Transform& /*transform*/,
                              std::uint32_t materialIdx) override {
        placements.emplace_back(meshIndex, materialIdx);
        ++instanceCount;
        return static_cast<std::uint32_t>(instanceCount - 1);
    }

    std::size_t materialCount = 0;
    std::size_t textureCount = 0;
    std::size_t meshCount = 0;
    std::size_t sphereMeshCount = 0;
    std::size_t instanceCount = 0;
    std::vector<aether::OpacityMicromapData> ommAssets;
    std::vector<sm::float3> materialColors;                          // per material index
    std::vector<std::pair<std::uint32_t, std::uint32_t>> placements; // (mesh index, material index)
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
        // Camera-presence contract (owner 2026-11-20): every loadable scene carries
        // a placement; this test's subject is the tonemapper fallback, so the scene
        // must otherwise be valid.
        out << "\n[camera]\ntranslate = [0.0, 1.0, 4.0]\nrotate_x = -10.0\nrotate_y = 0.0\n";
    }

    RecordingSceneBuilder scene;
    const harmonia::DeviceContext dummyCtx{};
    const harmonia::core::CommandPool dummyPool{};

    const auto cfg = harmonia::scene::SceneLoader::load(scenePath, assetsDir(), scene, dummyCtx, dummyPool);

    ASSERT_TRUE(cfg.has_value());
    EXPECT_FALSE(cfg->tonemapper.has_value());

    std::filesystem::remove_all(dir);
}

// Camera-presence contract (owner ruling 2026-11-20, Theia/PLAN "CAMERA PRESENCE
// CONTRACT"): a scene without a camera placement must fail the shared load - the
// private per-renderer fallbacks (Hyperion's Cornell constants, Theia's
// keep-internal-defaults path) were the bug this replaces. The scene below is
// otherwise valid (box mesh + instance), so the missing camera is the only reason
// the load fails.
TEST(SceneLoader, RejectsSceneWithoutCamera) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "harmonia_scene_loader_no_camera";
    std::filesystem::create_directories(dir);
    const std::filesystem::path scenePath = dir / "no_camera.scene.toml";
    {
        std::ofstream out(scenePath);
        out << "[[mesh]]\n"
               "name = \"box\"\n"
               "type = \"box\"\n"
               "half_extents = [1.0, 1.0, 1.0]\n"
               "\n"
               "[[instance]]\n"
               "mesh = \"box\"\n";
    }

    RecordingSceneBuilder scene;
    const harmonia::DeviceContext dummyCtx{};
    const harmonia::core::CommandPool dummyPool{};

    const auto cfg = harmonia::scene::SceneLoader::load(scenePath, assetsDir(), scene, dummyCtx, dummyPool);

    EXPECT_FALSE(cfg.has_value()) << "a scene without a camera placement must fail the load";
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
        // Camera-presence contract (owner 2026-11-20): the test's subject is the
        // stage toggles, so the scene is otherwise made valid.
        out << "\n[camera]\ntranslate = [0.0, 1.0, 4.0]\nrotate_x = -10.0\nrotate_y = 0.0\n";
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

// ── Per-sub-mesh material resolution ─────────────────────────────────────────
// A multi-material OBJ splits into sub-meshes at every `usemtl`. A Blender
// export names all of them after their object, so only the `usemtl` label tells
// them apart - a scene's per-sub-mesh keys must address either label.

/// Name a library color by its dominant channel. The primaries survive the
/// working-space conversion identifiable; the neutral fallback does not.
std::string classify(const sm::float3& c) {
    if (c.x > c.y && c.x > c.z) {
        return "R";
    }
    if (c.y > c.x && c.y > c.z) {
        return "G";
    }
    if (c.z > c.x && c.z > c.y) {
        return "B";
    }
    return "N";
}

TEST(GroupMaterialLoader, ResolvesPerSubmeshByUsemtlLabelThenGroupName) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "harmonia_group_materials";
    fs::create_directories(dir);
    {
        std::ofstream obj(dir / "part.obj");
        // A Blender-style multi-material export: one object, one usemtl per slot.
        obj << "o Part\n"
               "v 0 0 0\n"
               "v 1 0 0\n"
               "v 0 1 0\n"
               "v 2 0 0\n"
               "v 3 0 0\n"
               "v 2 1 0\n"
               "vn 0 0 1\n"
               "usemtl Frame\n"
               "f 1//1 2//1 3//1\n"
               "usemtl Glass\n"
               "f 4//1 5//1 6//1\n";
    }
    {
        std::ofstream lib(dir / "m.materials.toml");
        lib << "model = \"openpbr\"\n"
               "colorspace = \"lin_rec709_scene\"\n"
               "\n[Fallback]\n"
               "base_color = [0.1, 0.1, 0.1]\n"
               "\n[Red]\n"
               "base_color = [0.9, 0.05, 0.05]\n"
               "\n[Green]\n"
               "base_color = [0.05, 0.9, 0.05]\n"
               "\n[Blue]\n"
               "base_color = [0.05, 0.05, 0.9]\n";
    }
    {
        std::ofstream scn(dir / "s.scene.toml");
        scn << "material_libraries = \"m.materials.toml\"\n"
               "\n"
               "[camera]\n"
               "translate = [0.0, 1.0, 4.0]\n"
               "rotate_x = -10.0\n"
               "rotate_y = 0.0\n"
               "\n"
               "[[mesh]]\n"
               "name = \"part\"\n"
               "path = \"part.obj\"\n"
               "\n"
               "[[node]]\n"
               "id = \"by_usemtl\"\n"
               "mesh = \"part\"\n"
               "material = \"Fallback\"\n"
               "materials = { Frame = \"Red\", Glass = \"Green\" }\n"
               "\n"
               "[[node]]\n"
               "id = \"by_group\"\n"
               "mesh = \"part\"\n"
               "material = \"Fallback\"\n"
               "materials = { Part = \"Blue\" }\n"
               "\n"
               "[[node]]\n"
               "id = \"usemtl_wins\"\n"
               "mesh = \"part\"\n"
               "material = \"Fallback\"\n"
               "materials = { Part = \"Blue\", Glass = \"Green\" }\n"
               "\n"
               // A key naming no sub-mesh: diagnosed, the whole-mesh material stands.
               "[[node]]\n"
               "id = \"unknown_key\"\n"
               "mesh = \"part\"\n"
               "material = \"Fallback\"\n"
               "materials = { NoSuchGroup = \"Red\" }\n";
    }

    RecordingSceneBuilder scene;
    const harmonia::DeviceContext dummyCtx{};
    const harmonia::core::CommandPool dummyPool{};
    const auto cfg = harmonia::scene::SceneLoader::load(dir / "s.scene.toml", dir, scene, dummyCtx, dummyPool);

    ASSERT_TRUE(cfg.has_value());
    ASSERT_EQ(scene.meshCount, 2U) << "one sub-mesh per usemtl";
    ASSERT_EQ(scene.placements.size(), 8U) << "two sub-meshes per node";

    std::string resolved;
    for (const auto& [meshIndex, materialIdx] : scene.placements) {
        ASSERT_LT(materialIdx, scene.materialColors.size());
        resolved += classify(scene.materialColors[materialIdx]);
    }
    //                by_usemtl | by_group | usemtl_wins | unknown_key
    EXPECT_EQ(resolved, "RGBBBGNN");
    fs::remove_all(dir);
}

// ── ANI4: skin coverage binding (parser validates all but the vertex count,
// which only the import knows - these run against the recording builder, no GPU).

// Mini OBJ: unit quad, full v/vt/vn face refs -> 4 deduplicated vertices.
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

/// Skin scene over the quad; @p rows extra bone rows override the exact count
/// (0 = exact coverage) so the mismatch path can be driven deliberately.
std::string skinSceneToml(std::size_t vertexCount, std::ptrdiff_t rowDelta) {
    const std::size_t rows = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(vertexCount) + rowDelta);
    std::ostringstream os;
    // Camera-presence contract (owner 2026-11-20): fixtures that must LOAD carry a
    // placement, so the tests keep judging their own subjects (vertex coverage) -
    // the contract does not bend for fixtures, fixtures adapt to the contract.
    os << "[camera]\n"
          "translate = [0.0, 1.0, 4.0]\n"
          "rotate_x = -10.0\n"
          "rotate_y = 0.0\n"
          "\n"
          "[[mesh]]\n"
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
    for (std::size_t v = 0; v < rows; ++v) {
        os << (v == 0 ? "\n  " : ",\n  ") << "[0, 0, 0, 0]";
    }
    os << "\n]\nbone_weights = [";
    for (std::size_t v = 0; v < rows; ++v) {
        os << (v == 0 ? "\n  " : ",\n  ") << "[1, 0, 0, 0]";
    }
    os << "\n]\n";
    return os.str();
}

TEST(SkinLoader, AcceptsExactVertexCoverage) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "harmonia_skin_cover_ok";
    fs::create_directories(dir);
    writeQuadObj(dir / "q.obj");
    const std::size_t count = quadVertexCount(dir / "q.obj");
    ASSERT_GT(count, 0U);
    {
        std::ofstream scn(dir / "s.scene.toml");
        scn << skinSceneToml(count, 0);
    }

    RecordingSceneBuilder scene;
    const harmonia::DeviceContext dummyCtx{};
    const harmonia::core::CommandPool dummyPool{};
    const auto cfg = harmonia::scene::SceneLoader::load(dir / "s.scene.toml", dir, scene, dummyCtx, dummyPool);

    EXPECT_TRUE(cfg.has_value());
    EXPECT_EQ(scene.meshCount, 1U);
    EXPECT_EQ(scene.instanceCount, 1U);
    fs::remove_all(dir);
}

TEST(SkinLoader, RejectsVertexCoverageMismatch) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "harmonia_skin_cover_bad";
    fs::create_directories(dir);
    writeQuadObj(dir / "q.obj");
    const std::size_t count = quadVertexCount(dir / "q.obj");
    ASSERT_GT(count, 0U);
    {
        std::ofstream scn(dir / "s.scene.toml");
        scn << skinSceneToml(count, +1); // one vertex too many
    }

    RecordingSceneBuilder scene;
    const harmonia::DeviceContext dummyCtx{};
    const harmonia::core::CommandPool dummyPool{};
    const auto cfg = harmonia::scene::SceneLoader::load(dir / "s.scene.toml", dir, scene, dummyCtx, dummyPool);

    EXPECT_FALSE(cfg.has_value()) << "coverage mismatch must fail the load";
    fs::remove_all(dir);
}

TEST(SkinLoader, RejectsProceduralSphereSkin) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "harmonia_skin_sphere";
    fs::create_directories(dir);
    {
        std::ofstream scn(dir / "s.scene.toml");
        scn << "[[mesh]]\n"
               "name = \"ball\"\n"
               "type = \"sphere\"\n"
               "radius = 0.5\n"
               "\n"
               "[[node]]\n"
               "id = \"root\"\n"
               "\n"
               "[[node]]\n"
               "id = \"part\"\n"
               "mesh = \"ball\"\n"
               "\n"
               "[[skin]]\n"
               "node = \"part\"\n"
               "joints = [\"root\"]\n"
               "inverse_bind = [[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]]\n"
               "bone_indices = [[0, 0, 0, 0]]\n"
               "bone_weights = [[1, 0, 0, 0]]\n"
               "\n"
               // Camera-presence contract (owner 2026-11-20): the scene must be valid
               // apart from its subject, so the sphere-skin rejection stays the
               // reason this load fails (camera check runs before import).
               "[camera]\n"
               "translate = [0.0, 1.0, 4.0]\n"
               "rotate_x = -10.0\n"
               "rotate_y = 0.0\n";
    }

    RecordingSceneBuilder scene;
    const harmonia::DeviceContext dummyCtx{};
    const harmonia::core::CommandPool dummyPool{};
    const auto cfg = harmonia::scene::SceneLoader::load(dir / "s.scene.toml", dir, scene, dummyCtx, dummyPool);

    EXPECT_FALSE(cfg.has_value()) << "procedural sphere meshes carry no stable count - rejected";
    fs::remove_all(dir);
}

// ── ANI5: morph coverage binding (the row COUNT binds to the import) ────────

/// Morph scene over the quad (no nodes needed - [[morph]] references the mesh);
/// @p rowDelta skews the authored rows away from the exact vertex count.
std::string morphSceneToml(std::size_t vertexCount, std::ptrdiff_t rowDelta) {
    const std::size_t rows = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(vertexCount) + rowDelta);
    std::ostringstream os;
    // Camera-presence contract (owner 2026-11-20): same rationale as skinSceneToml -
    // the loadable shape carries a placement so the morph tests judge row counts.
    os << "[camera]\n"
          "translate = [0.0, 1.0, 4.0]\n"
          "rotate_x = -10.0\n"
          "rotate_y = 0.0\n"
          "\n"
          "[[mesh]]\n"
          "name = \"q\"\n"
          "path = \"q.obj\"\n"
          "\n"
          "[[instance]]\n"
          "mesh = \"q\"\n"
          "\n"
          "[[morph]]\n"
          "mesh = \"q\"\n"
          "name = \"bulge\"\n"
          "deltas = [";
    for (std::size_t v = 0; v < rows; ++v) {
        os << (v == 0 ? "\n  " : ",\n  ") << "[0, 0, 1]";
    }
    os << "\n]\n";
    return os.str();
}

TEST(MorphLoader, AcceptsExactVertexCoverage) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "harmonia_morph_cover_ok";
    fs::create_directories(dir);
    writeQuadObj(dir / "q.obj");
    const std::size_t count = quadVertexCount(dir / "q.obj");
    ASSERT_GT(count, 0U);
    {
        std::ofstream scn(dir / "s.scene.toml");
        scn << morphSceneToml(count, 0);
    }

    RecordingSceneBuilder scene;
    const harmonia::DeviceContext dummyCtx{};
    const harmonia::core::CommandPool dummyPool{};
    const auto cfg = harmonia::scene::SceneLoader::load(dir / "s.scene.toml", dir, scene, dummyCtx, dummyPool);

    EXPECT_TRUE(cfg.has_value());
    EXPECT_EQ(scene.meshCount, 1U);
    fs::remove_all(dir);
}

TEST(MorphLoader, RejectsVertexCoverageMismatch) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "harmonia_morph_cover_bad";
    fs::create_directories(dir);
    writeQuadObj(dir / "q.obj");
    const std::size_t count = quadVertexCount(dir / "q.obj");
    ASSERT_GT(count, 0U);
    {
        std::ofstream scn(dir / "s.scene.toml");
        scn << morphSceneToml(count, +1); // one vertex too many
    }

    RecordingSceneBuilder scene;
    const harmonia::DeviceContext dummyCtx{};
    const harmonia::core::CommandPool dummyPool{};
    const auto cfg = harmonia::scene::SceneLoader::load(dir / "s.scene.toml", dir, scene, dummyCtx, dummyPool);

    EXPECT_FALSE(cfg.has_value()) << "morph row count must bind to the imported vertex count";
    fs::remove_all(dir);
}

// ── Relative asset paths ─────────────────────────────────────────────────────

TEST(SceneLoader, ResolvesRelativePathsAgainstTheSceneDirectoryThenTheAssetsDir) {
    // A Blender export is one folder: the scene, its meshes and its material
    // library. The assets scenes share live in the assets directory. A relative
    // path in the scene names the scene's own folder first, the assets
    // directory second.
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "harmonia_scene_relative_paths";
    const fs::path assets = root / "assets";
    const fs::path exported = root / "export";
    fs::create_directories(assets);
    fs::create_directories(exported);
    writeQuadObj(exported / "own.obj");
    writeQuadObj(assets / "shared.obj");
    {
        std::ofstream lib(exported / "m.materials.toml");
        lib << "model = \"openpbr\"\n"
               "colorspace = \"lin_rec709_scene\"\n"
               "\n[Red]\n"
               "base_color = [0.9, 0.05, 0.05]\n";
    }
    {
        std::ofstream scn(exported / "s.scene.toml");
        scn << "material_libraries = \"m.materials.toml\"\n"
               "\n"
               "[camera]\n"
               "translate = [0.0, 1.0, 4.0]\n"
               "rotate_x = -10.0\n"
               "rotate_y = 0.0\n"
               "\n"
               "[[mesh]]\n"
               "name = \"own\"\n"
               "path = \"own.obj\"\n"
               "\n"
               "[[mesh]]\n"
               "name = \"shared\"\n"
               "path = \"shared.obj\"\n"
               "\n"
               "[[node]]\n"
               "id = \"a\"\n"
               "mesh = \"own\"\n"
               "material = \"Red\"\n"
               "\n"
               "[[node]]\n"
               "id = \"b\"\n"
               "mesh = \"shared\"\n"
               "material = \"Red\"\n";
    }

    RecordingSceneBuilder scene;
    const harmonia::DeviceContext dummyCtx{};
    const harmonia::core::CommandPool dummyPool{};
    // The assets directory holds neither the scene nor the export's own files.
    const auto cfg = harmonia::scene::SceneLoader::load(exported / "s.scene.toml", assets, scene, dummyCtx, dummyPool);

    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(scene.meshCount, 2U) << "the own mesh from the scene's folder, the shared one from the assets dir";
    ASSERT_EQ(scene.placements.size(), 2U);
    for (const auto& [meshIndex, materialIdx] : scene.placements) {
        // The library came from the export's folder - unknown, the material would be the neutral default.
        EXPECT_EQ(classify(scene.materialColors.at(materialIdx)), "R");
    }
    fs::remove_all(root);
}

} // namespace
