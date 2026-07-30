#ifndef HARMONIA_APP_APP_HPP
#define HARMONIA_APP_APP_HPP

#include <volk/volk.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdint>
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
#include "harmonia/core/VulkanHandle.hpp"
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

namespace harmonia {

/// Shared application host for Harmonia-based renderers.
///
/// Owns everything that is identical for every renderer: SDL window, Vulkan
/// context, swapchain, command pool, shared descriptor/pipeline layout, the
/// linear HDR working-space image, frame synchronisation (double-buffered
/// timeline + per-image binary semaphores), the tonemap/present display pass,
/// resize handling, offscreen capture (scene-referred EXR + tonemapped PNG)
/// and scene-file plumbing (SceneLoader, working color space, tonemapper,
/// IBL probe).
///
/// The pipeline contract: the injected IRenderer produces a *linear* image in
/// the scene-referred working color space (Rec.2020 or Rec.709 primaries);
/// the host's shared ToneMapper is the glue that converts it to the
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

    /// The renderer's scene the SceneLoader streams geometry into. Called
    /// during every loadScene(); recreate the scene in onSceneUnload().
    [[nodiscard]] virtual ISceneBuilder& sceneBuilder() noexcept = 0;

    /// Create renderer-specific resources (pipelines, passes, ...). Called
    /// once after the host bootstrap (context, swapchain, descriptors,
    /// tonemapper and HDR image all exist).
    [[nodiscard]] virtual bool onInitialize() = 0;

    /// Build renderer-side scene state (acceleration structures, descriptor
    /// updates, camera, IBL wiring via iblProbe()). The scene geometry has
    /// already been streamed into sceneBuilder().
    [[nodiscard]] virtual bool onSceneLoaded(const SceneLoader::SceneConfig& sceneConfig) = 0;

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
    [[nodiscard]] Context& context() noexcept { return m_context; }
    [[nodiscard]] const DeviceContext& deviceContext() const noexcept { return m_context.deviceContext(); }
    [[nodiscard]] const CommandPool& commandPool() const noexcept { return m_commandPool; }
    [[nodiscard]] Swapchain& swapchain() noexcept { return m_swapchain; }
    [[nodiscard]] Descriptors& descriptors() noexcept { return m_descriptors; }
    [[nodiscard]] Image& hdrImage() noexcept { return m_hdrImage; }
    [[nodiscard]] const std::optional<IblProbe>& iblProbe() const noexcept { return m_iblProbe; }
    [[nodiscard]] ColorSpace::WorkingColorSpace workingColorSpace() const noexcept { return m_workingColorSpace; }
    [[nodiscard]] std::uint32_t tonemapper() const noexcept { return m_tonemapper; }
    [[nodiscard]] std::uint32_t frameIndex() const noexcept { return m_frameIndex; }

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
    std::uint64_t renderSceneReferred();
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
    void rebuildStagePipeline();
    void applySceneStageConfig(const SceneLoader::SceneConfig& sceneConfig);
    [[nodiscard]] bool hasTonemapStage() const noexcept;
    [[nodiscard]] std::filesystem::path resolveScenePath(const std::filesystem::path& sceneFile) const;
    /// Current scene-output source for presentation/capture (HDR fallback when
    /// denoiser stage is disabled or unavailable).
    [[nodiscard]] const Image& sceneOutputImage() const noexcept;
    [[nodiscard]] VkPipelineStageFlags2 sceneOutputStageMask() noexcept;
    [[nodiscard]] VkAccessFlags2 sceneOutputAccessMask() noexcept;
    [[nodiscard]] std::uint64_t accumulationResetToken() const noexcept;
    /// Reset token for the A-SVGF temporal denoiser. Changes only on scene/resize/config
    /// changes — NOT on camera movement — so the denoiser can reproject history via motion
    /// vectors across camera motion (A-SVGF / SVGF design). AccumulationPass uses the
    /// separate accumulationResetToken which does change on camera movement.
    [[nodiscard]] std::uint64_t denoiserResetToken() const noexcept;

    Config m_config{};
    Config::StagePipeline m_defaultStages{};
    SDL_Window* m_window{};
    Context m_context{};
    CommandPool m_commandPool{};
    Swapchain m_swapchain{};
    Descriptors m_descriptors{};
    ToneMapper m_toneMapper{};
    OffscreenCapture m_offscreen; // offscreen-only: SDR capture (ToneMapper + 8-bit image)
    Image m_hdrImage{};
    Image m_denoisedImage{};
    std::vector<std::unique_ptr<IRenderPass>> m_sceneStages;
    std::vector<std::unique_ptr<IRenderPass>> m_displayStages;
    bool m_sceneOutputUsesDenoised = false;
    GreenScreenRenderer m_displayRenderer{};
    std::optional<IblProbe> m_iblProbe;

    ColorSpace::WorkingColorSpace m_workingColorSpace = ColorSpace::WorkingColorSpace::LinRec2020;
    std::uint32_t m_tonemapper = 0;

    FrameSync m_frameSync;
    std::uint64_t m_sceneEpoch = 1;
    std::uint64_t m_extentEpoch = 1;
    std::uint64_t m_stageEpoch = 1;
    std::uint64_t m_accumViewEpoch = 0;
    bool m_interactiveAccumulation = false;
    std::uint32_t m_frameIndex = 0;
    bool m_displayOverlayLogged = false;
    bool m_running = false;
};

} // namespace harmonia
#endif // HARMONIA_APP_APP_HPP
