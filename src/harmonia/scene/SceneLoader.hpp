#ifndef HARMONIA_SCENE_SCENELOADER_HPP
#define HARMONIA_SCENE_SCENELOADER_HPP

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <slang-math/slang-math.hpp>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/CommandPool.hpp"
#include "harmonia/scene/ISceneBuilder.hpp"
#include "harmonia/utils/ColorSpace.hpp"

namespace harmonia {

/// Loads a scene definition file (.scene.toml).
///
/// Parsing is owned by Aether (`aether::SceneParser`); see Aether's
/// SceneParser.hpp for the full TOML format reference (material_libraries,
/// [render] / [camera] / [tonemap] sections with optional `reference` presets,
/// and the [[mesh]] / [[instance]] instancing model).  SceneLoader resolves the
/// parsed `aether::SceneDesc` against the assets directory: it loads the
/// referenced material libraries and OBJ files, instantiates procedural
/// geometry, and uploads everything through the renderer's ISceneBuilder.
///
/// Each declared mesh is imported / uploaded / accelerated **once** (object
/// space); each [[instance]] then places a mesh with a transform + material.
/// N instances of one mesh share one BLAS (true GPU instancing).
///
/// Renderer-facing semantics:
///   - samples_per_pixel / max_depth        → SceneConfig::spp / maxDepth
///   - environment_map / environment_unit_nits → IBL panorama (EXR, converted to
///                                            the working color space on load)
///   - camera translate / rotate (or rotate_x/y/z) / vertical_field_of_view / ev100
///                                          → SceneConfig camera overrides (same TRS
///                                            shape as an instance; forward/up
///                                            are derived from the rotation quaternion)
///   - tonemapper ("aces" | "agx" | "reinhard" | "hable")
///                                          → SceneConfig::tonemapper enum value
///   - post_tonemap.renderer ("green_screen") → SceneConfig::postTonemapRenderer
///   - render-stage toggles (in [render] or its referenced preset):
///       enable_accumulation_stage / enable_denoiser_stage /
///       enable_tonemapper_stage   → SceneConfig stage toggle optionals
///   - working_color_space ("lin_rec2020_scene" | "lin_rec709_scene")
///                                          → SceneConfig::workingColorSpace
///   - meshes (OBJ / box / sphere) + instances with TRS (glTF T × R × S);
///     whether spheres are analytic or tessellated is the renderer's choice.
class SceneLoader {
  public:
    struct SceneConfig {
        std::optional<sm::float3> cameraPos;
        std::optional<sm::float3> cameraAt;
        std::optional<sm::float3> cameraUp;
        std::optional<float> cameraVfov;
        std::optional<float> cameraEv100; ///< physical camera EV100 override
        std::optional<std::uint32_t> spp;
        std::optional<std::uint32_t> maxDepth;
        std::optional<float> envUnitNits;                ///< cd/m² per unit EXR value (physical unit multiplier)
        std::optional<std::filesystem::path> envMapFile; ///< equirect EXR IBL path (relative to assetsDir)
        std::optional<std::uint32_t> tonemapper;         ///< Tonemapper enum value; std::nullopt → eACES default
        std::optional<std::string> postTonemapRenderer;  ///< display-referred renderer token
        std::optional<bool> accumulationStageEnabled;    ///< std::nullopt → App default
        std::optional<bool> denoiserStageEnabled;        ///< std::nullopt → App default
        std::optional<bool> tonemapperStageEnabled;      ///< std::nullopt → App default
        /// Scene-referred working color space all assets were converted into
        /// ([render] working_color_space; default linear Rec.2020).
        harmonia::ColorSpace::WorkingColorSpace workingColorSpace = harmonia::ColorSpace::WorkingColorSpace::LinRec2020;
    };

    /// Populate @p scene from @p sceneFile.
    /// Asset paths are resolved relative to @p assetsDir.
    /// Returns a SceneConfig with camera / render overrides on success,
    /// or VK_ERROR_INITIALIZATION_FAILED if the file cannot be opened or a
    /// referenced asset fails to load.
    [[nodiscard]] static std::expected<SceneConfig, VkResult> load(const std::filesystem::path& sceneFile,
                                                                   const std::filesystem::path& assetsDir,
                                                                   ISceneBuilder& scene,
                                                                   const DeviceContext& ctx,
                                                                   const CommandPool& pool);
};

} // namespace harmonia

#endif // HARMONIA_SCENE_SCENELOADER_HPP
