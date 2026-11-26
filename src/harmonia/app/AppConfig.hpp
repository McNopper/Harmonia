#ifndef HARMONIA_APP_APPCONFIG_HPP
#define HARMONIA_APP_APPCONFIG_HPP

#include <cstdint>
#include <filesystem>
#include <string>

#include "harmonia/pipeline/DenoiserDefaults.hpp"
#include "harmonia/pipeline/PassContext.hpp"

namespace harmonia::app {

struct AppConfig {
    struct StagePipeline {
        bool accumulation = true;
        bool denoiser = true;
        bool tonemapper = true;
    };
    struct DenoiserOptions {
        float strength = pipeline::kDenoiserStrength;
        std::uint32_t iterations = pipeline::kDenoiserIterations;
        bool useHistory = true;
        float historyBlend = pipeline::kDenoiserHistoryBlend;
        bool useGradient = true; ///< A-SVGF adaptive temporal filtering (gradient-driven)
        float gradientAlpha = pipeline::kDenoiserGradientAlpha;
    };

    std::string title = "Harmonia";
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    /// Demo window size used by BOTH renderer mains (Theia and Hyperion).
    /// Hoisted by the cross-renderer audit decision (2026-11-19): the literal pair
    /// 1024x768 was written into two separate main.cpp files - two copies that could
    /// drift exactly the way the camera fallbacks did. Values unchanged; only the
    /// second copy is gone. The struct's generic default above stays 1920x1080.
    static constexpr std::uint32_t kDemoWidth = 1024;
    static constexpr std::uint32_t kDemoHeight = 768;
    /// Vulkan validation layers: ALWAYS ENABLED BY DEFAULT (family contract, 2026-11-15).
    /// Context creation hard-fails without VK_LAYER_KHRONOS_validation, the test
    /// fixtures all set enableValidation = true, and no family tool passes
    /// --no-validation anymore (every render is validated — the dedicated zero-warning
    /// gate is tools/check_vulkan_validation.py). Keep it this way: this session's
    /// bugs (a push-constant block emitted as a descriptor, a missing TRANSFER_DST
    /// usage) were only visible under validation.
    bool validation = true;
    /// Allow interactive window resizing. Renderers whose passes cannot
    /// recreate their targets yet should keep this off.
    bool resizable = true;
    std::filesystem::path assetsDir;  ///< canonical Aether asset collection
    std::filesystem::path sceneFile;  ///< a path, or a name / subdirectory path inside assetsDir
    std::filesystem::path outputFile; ///< non-empty: offscreen render (EXR + PNG), then exit
    /// Force off all post-processing stages (the shared denoiser/A-SVGF stage and
    /// renderer-side TAA) so the displayed image is the raw estimator result — the
    /// same identity --output enforces for offscreen capture, available in the
    /// interactive window.
    bool noPostfx = false;
    /// Number of scene-referred frames to render before saving in offscreen mode.
    /// For stochastic pipelines, increase this to improve convergence.
    std::uint32_t offscreenFrames = 4;
    /// ANI7 sequence render: >0 renders that many ANIMATION frames to disk instead of
    /// one accumulated capture - per output frame k the animation tick is pinned to k
    /// (t = k / animationFps, box shutter at the frame start; constant across the
    /// output frame's accumulation samples), accumulation restarts, and EXR+PNG are
    /// written as `<output-stem>_0000<ext>` ... `<output-stem>_<k><ext>`.
    /// 0 (default) = the classic single capture. Requires --output.
    std::uint32_t sequenceFrames = 0;
    /// ANI8 `--frame N`: animation START frame - animationFrameIndex() adds this
    /// offset, so sequence output frame k renders tick N + k, the interactive
    /// window starts at N and runs in real time, and a classic single capture
    /// 0 (default) = unchanged behavior.
    std::uint32_t animationStartFrame = 0;
    /// ANI7 timing extension: capture every N milliseconds of ANIMATION TIME
    /// (requires --sequence-frames). Output frame k then samples
    /// start + k * interval SECONDS instead of the integer tick grid, so
    /// sub-frame intervals (every 10 ms at 30 fps) and multi-frame intervals
    /// (every 1000 ms) are both expressible. 0 (default) = the tick grid
    /// (t = tick / fps), unchanged. The range is start + count * interval;
    /// count stays --sequence-frames.
    double captureIntervalMs = 0.0;
    /// Explicit capture start in SECONDS (>= 0); negative (default) = derive
    /// it from --frame / fps as before.
    double captureStartSeconds = -1.0;
    /// Interactive window: accumulation frames per swapchain flip (1 = one render per
    /// present, the classic cadence). N > 1 converges the DISPLAYED image faster per
    /// flip on a static/slow camera at the cost of flip rate and input latency;
    /// accumulation still resets on camera move. The interactive-window analogue of
    /// --offscreen-frames (which is the headless equivalent).
    std::uint32_t framesPerFlip = 1;
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
    std::uint32_t rngSeed = pipeline::kDefaultRngSeed;
    /// Optional stochastic debug path switch for renderer-side visualization/tests.
    bool rngDebug = false;
};

} // namespace harmonia::app
#endif // HARMONIA_APP_APPCONFIG_HPP
