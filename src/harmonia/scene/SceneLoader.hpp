#pragma once

#include <glm/glm.hpp>

#include <filesystem>
#include <optional>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/CommandPool.hpp"
#include "harmonia/scene/ISceneBuilder.hpp"
#include "harmonia/utils/ColorSpace.hpp"

/// Loads a scene definition file (.scene.toml).
///
/// Parsing is owned by Aether (`aether::SceneParser`); see Aether's
/// SceneParser.hpp for the full TOML format reference (material_libraries,
/// [render] / [camera] / [tonemap] sections with optional `reference` presets,
/// and ordered [[geometry]] blocks).  SceneLoader resolves the parsed
/// `aether::SceneDesc` against the assets directory: it loads the referenced
/// material libraries and OBJ files, instantiates procedural geometry, and
/// uploads everything through the renderer's ISceneBuilder.
///
/// Renderer-facing semantics:
///   - samples_per_pixel / max_depth        → SceneConfig::spp / maxDepth
///   - environment_map / environment_unit_nits → IBL panorama (EXR, converted to
///                                            the working color space on load)
///   - camera translate / look_at / up / vertical_field_of_view / ev100
///                                          → SceneConfig camera overrides
///   - tonemapper ("aces" | "agx" | "reinhard" | "hable")
///                                          → SceneConfig::tonemapper enum value
///   - post_tonemap.renderer ("green_screen") → SceneConfig::postTonemapRenderer
///   - render-stage toggles (in [render] or its referenced preset):
///       enable_accumulation_stage / enable_denoiser_stage /
///       enable_tonemapper_stage   → SceneConfig stage toggle optionals
///   - working_color_space ("lin_rec2020_scene" | "lin_rec709_scene")
///                                          → SceneConfig::workingColorSpace
///   - geometry: instance (OBJ), box, sphere with TRS (glTF T × R × S);
///     whether spheres are analytic or tessellated is the renderer's choice.
class SceneLoader {
  public:
    struct SceneConfig {
        std::optional<glm::vec3> cameraPos;
        std::optional<glm::vec3> cameraAt;
        std::optional<glm::vec3> cameraUp;
        std::optional<float> cameraVfov;
        std::optional<float> cameraEv100; ///< physical camera EV100 override
        std::optional<uint32_t> spp;
        std::optional<uint32_t> maxDepth;
        std::optional<float> envUnitNits;                ///< cd/m² per unit EXR value (physical unit multiplier)
        std::optional<std::filesystem::path> envMapFile; ///< equirect EXR IBL path (relative to assetsDir)
        std::optional<uint32_t> tonemapper;              ///< Tonemapper enum value; std::nullopt → eACES default
        std::optional<std::string> postTonemapRenderer;  ///< display-referred renderer token
        std::optional<bool> accumulationStageEnabled;    ///< std::nullopt → App default
        std::optional<bool> denoiserStageEnabled;        ///< std::nullopt → App default
        std::optional<bool> tonemapperStageEnabled;      ///< std::nullopt → App default
        /// Scene-referred working color space all assets were converted into
        /// ([render] working_color_space; default linear Rec.2020).
        ColorSpace::WorkingColorSpace workingColorSpace = ColorSpace::WorkingColorSpace::LinRec2020;
    };

    /// Populate @p scene from @p sceneFile.
    /// Asset paths are resolved relative to @p assetsDir.
    /// Returns a SceneConfig with camera / render overrides on success,
    /// or std::nullopt if the file cannot be opened.
    [[nodiscard]] static std::optional<SceneConfig> load(const std::filesystem::path& sceneFile,
                                                         const std::filesystem::path& assetsDir,
                                                         ISceneBuilder& scene,
                                                         const DeviceContext& ctx,
                                                         const CommandPool& pool);
};
