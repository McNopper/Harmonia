#ifndef HARMONIA_APP_APPCONFIG_HPP
#define HARMONIA_APP_APPCONFIG_HPP

#include <cstdint>
#include <filesystem>
#include <string>

namespace harmonia {

struct AppConfig {
    struct StagePipeline {
        bool accumulation = true;
        bool denoiser = true;
        bool tonemapper = true;
    };
    struct DenoiserOptions {
        float strength = 0.45F;
        std::uint32_t iterations = 2U;
        bool useHistory = true;
        float historyBlend = 0.15F;
        bool useGradient = true;    ///< A-SVGF adaptive temporal filtering (gradient-driven)
        float gradientAlpha = 0.2F; ///< temporal blend factor for the A-SVGF gradient
    };

    std::string title = "Harmonia";
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    bool validation = true;
    /// Allow interactive window resizing. Renderers whose passes cannot
    /// recreate their targets yet should keep this off.
    bool resizable = true;
    std::filesystem::path assetsDir;  ///< canonical Aether asset collection
    std::filesystem::path sceneFile;  ///< bare names are resolved against assetsDir (+ ".scene.toml")
    std::filesystem::path outputFile; ///< non-empty: offscreen render (EXR + PNG), then exit
    /// Force off all post-processing stages (the shared denoiser/A-SVGF stage and
    /// renderer-side TAA) so the displayed image is the raw estimator result — the
    /// same identity --output enforces for offscreen capture, available in the
    /// interactive window.
    bool noPostfx = false;
    /// Number of scene-referred frames to render before saving in offscreen mode.
    /// For stochastic pipelines, increase this to improve convergence.
    std::uint32_t offscreenFrames = 4;
    /// Presentation-only indirect ambient boost (scene-referred linear units).
    /// Kept at 0.0 for parity fixtures; non-zero values are for interactive
    /// quality tuning only.
    float indirectAmbient = 0.0f;
    /// IBL diffuse irradiance atlas width; height is width / 2 (2:1 lat-long).
    std::uint32_t iblDiffuseResolution = 256;
    /// Optional post-tonemap display-referred overlay renderer.
    bool displayOverlay = false;
    /// Config-driven stage toggles (scene/render preset overrides may update
    /// these when a scene is loaded).
    StagePipeline stages{};
    /// Shared denoiser controls for the scene-output stage.
    DenoiserOptions denoiser{};
    /// Enables replayable frame/sample RNG sequencing for stochastic stages.
    bool deterministicReplay = false;
    /// Base seed used by renderer RNG composition (pixel + frame/sample + bounce).
    std::uint32_t rngSeed = 0x12345678U;
    /// Optional stochastic debug path switch for renderer-side visualization/tests.
    bool rngDebug = false;
};

} // namespace harmonia
#endif // HARMONIA_APP_APPCONFIG_HPP
