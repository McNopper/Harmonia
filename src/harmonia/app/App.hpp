#ifndef HARMONIA_APP_APP_HPP
#define HARMONIA_APP_APP_HPP

#include <volk/volk.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "harmonia/app/AppConfig.hpp"
#include "harmonia/app/CliParser.hpp"
#include "harmonia/app/FrameSync.hpp"
#include "harmonia/app/GreenScreenRenderer.hpp"
#include "harmonia/app/IRenderer.hpp"
#include "harmonia/app/OffscreenCapture.hpp"
#include "harmonia/core/CommandPool.hpp"
#include "harmonia/core/Image.hpp"
#include "harmonia/core/PipelineBinaryCache.hpp"
#include "harmonia/core/VulkanHandle.hpp"
#include "harmonia/pipeline/GlarePass.hpp"
#include "harmonia/pipeline/IRenderPass.hpp"
#include "harmonia/pipeline/SceneOutputCopyPass.hpp"
#include "harmonia/presentation/Swapchain.hpp"
#include "harmonia/presentation/ToneMapper.hpp"
#include "harmonia/renderer/Descriptors.hpp"
#include "harmonia/scene/ISceneBuilder.hpp"
#include "harmonia/scene/IblProbe.hpp"
#include "harmonia/scene/SceneLoader.hpp"
#include "harmonia/utils/ColorSpace.hpp"
#include "harmonia/vulkan_init/Context.hpp"

namespace harmonia::app {

/// Shared application host for Harmonia-based renderers.
///
/// Owns everything that is identical for every renderer: SDL window, Vulkan
/// context, swapchain, command pool, shared descriptor/pipeline layout, the
/// linear HDR working-space image, frame synchronisation (double-buffered
/// timeline + per-image binary semaphores), the tonemap/present display pass,
/// resize handling, offscreen capture (scene-referred EXR + tonemapped PNG)
/// and scene-file plumbing (scene::SceneLoader, working color space, tonemapper,
/// IBL probe).
///
/// The pipeline contract: the injected IRenderer produces a *linear* image in
/// the scene-referred working color space (Rec.2020 or Rec.709 primaries);
/// the host's shared presentation::ToneMapper is the glue that converts it to the
/// display-referred output (HDR10/HLG/scRGB/P3/SDR). Hyperion and Theia only
/// implement the hooks below and inject their renderer.
class App {
  public:
    using Config = AppConfig;

    App() = default;
    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) = delete;
    App& operator=(App&&) = delete;
    virtual ~App();

    /// Bootstrap, load the scene, then run offscreen capture or the
    /// interactive loop. Returns the process exit code.
    int run(Config&& config);

  protected:
    // ── Hooks (renderer injection) ──────────────────────────────────────────

    /// The injected scene-referred renderer (e.g. Hyperion's path tracer,
    /// Theia's forward stack). Valid after onInitialize() succeeded.
    [[nodiscard]] virtual IRenderer& renderer() noexcept = 0;

    /// The renderer's scene the scene::SceneLoader streams geometry into. Called
    /// during every loadScene(); recreate the scene in onSceneUnload().
    [[nodiscard]] virtual scene::ISceneBuilder& sceneBuilder() noexcept = 0;

    /// Create renderer-specific resources (pipelines, passes, ...). Called
    /// once after the host bootstrap (context, swapchain, descriptors,
    /// tonemapper and HDR image all exist).
    [[nodiscard]] virtual bool onInitialize() = 0;

    /// Build renderer-side scene state (acceleration structures, descriptor
    /// updates, camera, IBL wiring via iblProbe()). The scene geometry has
    /// already been streamed into sceneBuilder().
    [[nodiscard]] virtual bool onSceneLoaded(const scene::SceneLoader::SceneConfig& sceneConfig) = 0;

    /// Tear down scene state before a (re)load. Default: no-op. The host has
    /// already waited for the device to idle.
    virtual void onSceneUnload() {}

    /// Renderer-specific input. Return true when the event was consumed
    /// (e.g. to override the default ESC-quits behaviour).
    virtual bool onEvent(const SDL_Event& event) {
        static_cast<void>(event);
        return false;
    }

    /// Called after renderer().record() and before the scene stages are recorded.
    /// The default returns {renderCmd, VK_NULL_HANDLE} for the single-queue path.
    /// Subclasses may override to split the frame across an async compute queue:
    ///   - end renderCmd and submit it to the graphics queue (signalling a semaphore)
    ///   - dispatch async compute work on a separate queue
    ///   - begin and return a fresh graphics-family command buffer that the caller
    ///     will use to record the scene stages (denoiser etc.)
    /// @return {stagesCmd, asyncWaitSem}: cmd for scene stages and an optional binary
    ///         semaphore the final stages submit waits on (VK_NULL_HANDLE = no wait)
    [[nodiscard]] virtual std::pair<VkCommandBuffer, VkSemaphore>
    onBeforeSceneStages(VkCommandBuffer renderCmd) noexcept {
        return {renderCmd, VK_NULL_HANDLE};
    }

    /// A subclass coordinating extra producer passes (e.g. async GI) may supply
    /// their completion adapter instead of the injected renderer's adapter.
    [[nodiscard]] virtual IFrameCompletion* sceneFrameCompletion() noexcept { return renderer().frameCompletion(); }

    /// Producer work is now complete. Record any dependent renderer-owned post
    /// passes here; the shared scene stages follow. Default preserves old order.
    virtual void onSceneFrameReady(VkCommandBuffer cmd) noexcept { static_cast<void>(cmd); }

    /// Per-frame update (camera controller, window title, ...).
    virtual void onUpdate(float dtSeconds) { static_cast<void>(dtSeconds); }

    /// Called after the host completed a resize (swapchain, HDR image and
    /// tonemapper recreated, renderer().onResize() already invoked).
    virtual void onResized(VkExtent2D extent) { static_cast<void>(extent); }

    /// Number of frames to record for an offscreen capture (--output) before
    /// the image is saved (samples for an accumulating path tracer, warmup
    /// frames for a real-time renderer).
    [[nodiscard]] virtual std::uint32_t offscreenFrameCount() const noexcept {
        return std::max(m_config.offscreenFrames, 1U);
    }

    // ── Services for subclasses ─────────────────────────────────────────────

    [[nodiscard]] const Config& config() const noexcept { return m_config; }

    /// Enable progressive accumulation in the interactive (windowed) path so a
    /// stationary view converges instead of showing each raw, per-frame
    /// stochastic sample (camera jitter, env/shadow sampling, GI). The renderer
    /// must call resetAccumulation() whenever the view changes (camera move,
    /// exposure change, ...) so accumulation restarts. Off by default; the
    /// offscreen capture path always accumulates regardless of this flag.
    void setInteractiveAccumulation(bool enabled) noexcept { m_interactiveAccumulation = enabled; }

    /// Restart progressive accumulation from the next frame (drop the converged
    /// history). Call when the camera moves or any view-affecting parameter
    /// changes. No-op cost when interactive accumulation is disabled.
    void resetAccumulation() noexcept { ++m_accumViewEpoch; }

    [[nodiscard]] SDL_Window* window() const noexcept { return m_window; }
    [[nodiscard]] vulkan_init::Context& context() noexcept { return m_context; }
    [[nodiscard]] const DeviceContext& deviceContext() const noexcept { return m_context.deviceContext(); }
    [[nodiscard]] const core::CommandPool& commandPool() const noexcept { return m_commandPool; }
    [[nodiscard]] presentation::Swapchain& swapchain() noexcept { return m_swapchain; }
    [[nodiscard]] renderer::Descriptors& descriptors() noexcept { return m_descriptors; }
    [[nodiscard]] core::Image& hdrImage() noexcept { return m_hdrImage; }
    [[nodiscard]] const std::optional<scene::IblProbe>& iblProbe() const noexcept { return m_iblProbe; }
    [[nodiscard]] utils::ColorSpace::WorkingColorSpace workingColorSpace() const noexcept {
        return m_workingColorSpace;
    }
    [[nodiscard]] std::uint32_t tonemapper() const noexcept { return m_tonemapper; }
    [[nodiscard]] std::uint32_t frameIndex() const noexcept { return m_frameIndex; }

    /// ANI7: the logical ANIMATION tick index the renderer's per-frame animation
    /// update (setAnimationTime = index / animationFps) must use. Sequence render
    /// (--sequence-frames) pins it to the OUTPUT frame index so every accumulation
    /// sample of output frame k renders the same scene state; otherwise it is the
    /// raw per-render-frame index (ANI2's deterministic tick, unchanged).
    /// ANI8 `--frame N`: the whole clock is offset by config.animationStartFrame,
    /// so output frame k renders tick N + k (movie frame generation) and the
    /// interactive window starts at N.
    [[nodiscard]] std::uint32_t animationFrameIndex() const noexcept {
        return m_config.animationStartFrame + (m_sequenceActive ? m_sequenceFrame : m_frameIndex);
    }

  public:
    /// THE animation clock - the ONE source of animation time for EVERY
    /// consumer: scene stamping, deformation, TLAS refit and the scripted
    /// camera. One call derives the time AND its change key together, so a
    /// key/seconds pair can never disagree.
    struct AnimationClock {
        double seconds = 0.0;  ///< animation time this output frame renders at
        std::uint64_t key = 0; ///< monotonic change key for the guards
    };

    /// The animation time one accumulation sample of one output frame renders
    /// at in a sequence capture. Pure: the same arguments give the same result,
    /// so a capture is reproducible from --rng-seed.
    ///
    /// The output frame's own time comes from the capture mode:
    ///   * interval capture (--capture-every-ms, sequence mode only):
    ///     start + outputFrame * interval, start = --start-seconds else
    ///     --frame / fps - sub-frame and multi-frame sampling alike;
    ///   * otherwise the tick grid: (--frame + outputFrame) / fps.
    ///
    /// With a shutter (--shutter > 0 and at least two --time-samples) the
    /// frame's samples are spread over the open shutter: sample i falls in the
    /// i-th of timeSamples equal strata of [frame time, frame time + shutter x
    /// frame interval), at a jittered position inside its stratum. Stratified
    /// and jittered, so the exposure integral is unbiased for any motion and
    /// better distributed than white noise. The frame interval is the capture
    /// interval, or 1 / fps on the tick grid. The jitter depends on the RNG
    /// seed, the output frame index and the stratum: two captures with the
    /// same seed, in either renderer, integrate the same instants.
    ///
    /// `key` changes iff the sampled time changes (microsecond-quantized in
    /// interval and shutter mode, the tick index otherwise).
    [[nodiscard]] static AnimationClock
    sequenceSampleClock(const AppConfig& config, double fps, std::uint32_t outputFrame, std::uint32_t sample) noexcept;

    /// The clock for the sample being rendered. Outside a sequence capture it
    /// is the raw per-render-frame tick (deterministic; interactive use).
    [[nodiscard]] AnimationClock animationClock(double fps) const noexcept {
        if (m_sequenceActive) {
            return sequenceSampleClock(m_config, fps, m_sequenceFrame, m_sequenceSample);
        }
        const double safeFps = fps > 0.0 ? fps : 30.0;
        return AnimationClock{.seconds = static_cast<double>(animationFrameIndex()) / safeFps,
                              .key = animationFrameIndex()};
    }

    /// True while a sequence capture renders. The capture restarts
    /// accumulation once per output frame and owns that restart, so a renderer
    /// must not restart it again inside one: with a shutter the animation tick
    /// and the camera change from sample to sample within a frame, and all of
    /// those samples accumulate into the one exposure.
    [[nodiscard]] bool sequenceCaptureActive() const noexcept { return m_sequenceActive; }

  protected:
    /// Resolve + load a scene file (full path or bare name). Reusable at
    /// runtime for scene switching.
    [[nodiscard]] bool loadScene(const std::filesystem::path& sceneFile);

    /// Stop the interactive loop after the current frame.
    void requestQuit() noexcept { m_running = false; }

    /// Save the current HDR image as a scene-referred EXR (untonemapped,
    /// chromaticities-tagged). The matching tonemapped PNG is written by the
    /// offscreen path only.
    bool saveExr(const std::filesystem::path& path);

    /// A3(b): returns the A-SVGF gradient/variance image view from the shared denoiser pass,
    /// or VK_NULL_HANDLE when the denoiser is disabled or gradient tracking is off.
    /// Renderers can pass this to their GI pass for per-pixel adaptive firefly clamping.
    [[nodiscard]] VkImageView denoiserGradientImageView() const noexcept;

  private:
    [[nodiscard]] bool bootstrap();
    void shutdown() noexcept;
    int mainLoop();
    [[nodiscard]] int renderOffscreen(); /// Record + submit the renderer into the HDR image; returns the timeline
    /// value signalled on completion and advances the frame slot.
    /// ANI7: write the EXR + tone-mapped PNG pair for one captured output frame to
    /// @p outputFile (the classic single-capture path passes config().outputFile;
    /// sequence render passes the per-frame path).
    bool saveOffscreenOutputs(const std::filesystem::path& outputFile);
    std::expected<std::uint64_t, VkResult> renderSceneReferred();
    /// Acquire, tonemap, present the given completed scene-referred frame.
    void presentFrame(std::uint32_t slot, std::uint64_t renderValue);
    [[nodiscard]] std::optional<std::uint32_t> acquireFrame(std::uint32_t slot) noexcept;
    void recordDisplayBarriers(VkCommandBuffer cmd, std::uint32_t imageIndex) noexcept;
    [[nodiscard]] VkResult
    submitDisplay(std::uint32_t slot, std::uint32_t imageIndex, std::uint64_t renderValue) noexcept;
    void presentAndHandleResize(std::uint32_t imageIndex) noexcept;
    void handleResize(std::uint32_t w, std::uint32_t h);
    [[nodiscard]] bool createHdrImage();
    [[nodiscard]] bool createDenoisedImage();
    [[nodiscard]] bool createToneMapper();
    /// One-shot diagnostic: when VK_KHR_calibrated_timestamps is supported, sample the
    /// GPU<->host clock relationship and log it (exercises the extension's entry points).
    void logGpuClockCalibration() noexcept;
    void logGpuMemoryBudget() noexcept;

    // VK18 (VK_EXT_present_timing) rolling cadence statistics — telemetry only.
    [[nodiscard]] bool rebuildStagePipeline();
    /// Applies the scene's stage toggles on top of the CLI defaults; enforces
    /// the two-tier output contract (no denoiser for --output / --no-postfx).
    /// Returns false on a contract violation — the scene load is aborted.
    [[nodiscard]] bool applySceneStageConfig(const scene::SceneLoader::SceneConfig& sceneConfig);
    [[nodiscard]] bool hasTonemapStage() const noexcept;
    [[nodiscard]] std::filesystem::path resolveScenePath(const std::filesystem::path& sceneFile) const;
    /// Current scene-output source for presentation/capture (HDR fallback when
    /// denoiser stage is disabled or unavailable).
    [[nodiscard]] const core::Image& sceneOutputImage() const noexcept;
    [[nodiscard]] VkPipelineStageFlags2 sceneOutputStageMask() noexcept;
    [[nodiscard]] VkAccessFlags2 sceneOutputAccessMask() noexcept;
    [[nodiscard]] std::uint64_t accumulationResetToken() const noexcept;
    /// Reset token for the A-SVGF temporal denoiser. Changes only on scene/resize/config
    /// changes — NOT on camera movement — so the denoiser can reproject history via motion
    /// vectors across camera motion (A-SVGF / SVGF design). pipeline::AccumulationPass uses the
    /// separate accumulationResetToken which does change on camera movement.
    [[nodiscard]] std::uint64_t denoiserResetToken() const noexcept;

    // VK18 (VK_EXT_present_timing) rolling cadence statistics — telemetry only.
    std::uint64_t m_presentTimingSamples = 0;
    std::uint64_t m_presentTimingIntervalSumNs = 0;
    std::uint64_t m_presentTimingIntervalMaxNs = 0;
    std::uint64_t m_presentTimingIncomplete = 0;
    std::uint64_t m_presentTimingLastNs = 0;

    // Field order follows the analyzer's alignment-pack suggestion, with ONE
    // semantic exception kept: m_context stays immediately before
    // m_pipelineBinaryCache so the cache outlives the DeviceContext whose
    // non-owning pointer the pipeline-creation sites dispatch through (same
    // lifetime contract as before; reordering the other fields is layout-only).
    SDL_Window* m_window{};
    std::uint64_t m_sceneEpoch = 1;
    std::uint64_t m_extentEpoch = 1;
    std::uint64_t m_stageEpoch = 1;
    std::uint64_t m_accumViewEpoch = 0;
    /// Pacing diagnostic counters (I6 verification, TASK: pacing oracle): every 300
    /// rendered frames mainLoop() logs the rendered/flips ratio — the measurable
    /// frames-per-flip contract check (N=1 → R≈F, N=4 → F≈R/4).
    std::uint64_t m_pacingRenderedFrames = 0;
    std::uint64_t m_pacingFlippedFrames = 0;
    std::vector<std::unique_ptr<pipeline::IRenderPass>> m_sceneStages;
    std::vector<std::unique_ptr<pipeline::IRenderPass>> m_displayStages;
    GreenScreenRenderer m_displayRenderer{};
    core::CommandPool m_commandPool{};
    vulkan_init::Context m_context{};
    // VK5: opt-in explicit pipeline-binary cache (HARMONIA_PIPELINE_BINARY_CACHE=<dir>).
    // Kept immediately after m_context for the lifetime contract above.
    std::optional<core::PipelineBinaryCache> m_pipelineBinaryCache;
    presentation::ToneMapper m_toneMapper{};
    /// The lens glare of the display stages, recorded right before the tone
    /// mapper: owned by m_displayStages, null unless `--glare` is above 0. A
    /// capture records it for its PNG too; the EXR is written before it.
    pipeline::GlarePass* m_glarePass = nullptr;
    core::Image m_hdrImage{};
    core::Image m_denoisedImage{};
    OffscreenCapture m_offscreen; // offscreen-only: SDR capture (presentation::ToneMapper + 8-bit image)
    FrameSync m_frameSync;
    presentation::Swapchain m_swapchain{};
    Config m_config{};
    std::optional<scene::IblProbe> m_iblProbe;
    renderer::Descriptors m_descriptors{};

    utils::ColorSpace::WorkingColorSpace m_workingColorSpace = utils::ColorSpace::WorkingColorSpace::LinRec2020;
    std::uint32_t m_tonemapper = 0;

    std::uint32_t m_frameIndex = 0;
    /// ANI7 sequence render state (set by renderOffscreen, read by
    /// animationFrameIndex): the current OUTPUT frame + active flag.
    std::uint32_t m_sequenceFrame = 0;
    /// Accumulation sample of the output frame being rendered (0..count-1);
    /// with a shutter it selects the sample's time inside the frame.
    std::uint32_t m_sequenceSample = 0;
    /// Interactive flip cadence counter for --frames-per-flip (I6).
    std::uint32_t m_framesSinceFlip = 0;
    bool m_presentTimingLog = false;
    bool m_sceneOutputUsesDenoised = false;
    bool m_stagePipelineFailed = false;
    bool m_interactiveAccumulation = false;
    bool m_sequenceActive = false;
    bool m_displayOverlayLogged = false;
    bool m_running = false;
    Config::StagePipeline m_defaultStages{};
};

} // namespace harmonia::app
#endif // HARMONIA_APP_APP_HPP
