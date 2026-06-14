#pragma once

#include <volk/volk.h>

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "harmonia/app/IRenderer.hpp"
#include "harmonia/core/CommandPool.hpp"
#include "harmonia/core/Image.hpp"
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
    struct Config {
        std::string title = "Harmonia";
        uint32_t width = 1920;
        uint32_t height = 1080;
        bool validation = true;
        /// Allow interactive window resizing. Renderers whose passes cannot
        /// recreate their targets yet should keep this off.
        bool resizable = true;
        std::filesystem::path assetsDir;  ///< canonical Aether asset collection
        std::filesystem::path sceneFile;  ///< bare names are resolved against assetsDir (+ ".scene.toml")
        std::filesystem::path outputFile; ///< non-empty: offscreen render (EXR + PNG), then exit
        /// Screen-space post-effects (SSR/SSAO/bloom). Disabled (--no-postfx) for
        /// parity comparison renders, which require all post-effects off per the
        /// locked comparison contract. Renderers without post-fx ignore this.
        bool postProcess = true;
    };

    App() = default;
    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) = delete;
    App& operator=(App&&) = delete;
    virtual ~App();

    /// Bootstrap, load the scene, then run offscreen capture or the
    /// interactive loop. Returns the process exit code.
    int run(Config config);

    /// Parses an argument the host understands (--scene/-s, --output/-o,
    /// --width, --height, --validation, --no-validation, --no-postfx, or a
    /// bare scene name). Returns true if consumed; @p i may be advanced.
    [[nodiscard]] static bool applyCommonArg(Config& config, int& i, int argc, char* const argv[]);

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

    /// Per-frame update (camera controller, window title, ...).
    virtual void onUpdate(float dtSeconds) { static_cast<void>(dtSeconds); }

    /// Called after the host completed a resize (swapchain, HDR image and
    /// tonemapper recreated, renderer().onResize() already invoked).
    virtual void onResized(VkExtent2D extent) { static_cast<void>(extent); }

    /// Number of frames to record for an offscreen capture (--output) before
    /// the image is saved (samples for an accumulating path tracer, warmup
    /// frames for a real-time renderer).
    [[nodiscard]] virtual uint32_t offscreenFrameCount() const noexcept { return 4; }

    // ── Services for subclasses ─────────────────────────────────────────────

    [[nodiscard]] const Config& config() const noexcept { return m_config; }
    [[nodiscard]] SDL_Window* window() const noexcept { return m_window; }
    [[nodiscard]] Context& context() noexcept { return m_context; }
    [[nodiscard]] const DeviceContext& deviceContext() const noexcept { return m_context.deviceContext(); }
    [[nodiscard]] const CommandPool& commandPool() const noexcept { return m_commandPool; }
    [[nodiscard]] Swapchain& swapchain() noexcept { return m_swapchain; }
    [[nodiscard]] Descriptors& descriptors() noexcept { return m_descriptors; }
    [[nodiscard]] Image& hdrImage() noexcept { return m_hdrImage; }
    [[nodiscard]] const std::optional<IblProbe>& iblProbe() const noexcept { return m_iblProbe; }
    [[nodiscard]] ColorSpace::WorkingColorSpace workingColorSpace() const noexcept { return m_workingColorSpace; }
    [[nodiscard]] uint32_t tonemapper() const noexcept { return m_tonemapper; }
    [[nodiscard]] uint32_t frameIndex() const noexcept { return m_frameIndex; }

    /// Resolve + load a scene file (full path or bare name). Reusable at
    /// runtime for scene switching.
    [[nodiscard]] bool loadScene(const std::filesystem::path& sceneFile);

    /// Stop the interactive loop after the current frame.
    void requestQuit() noexcept { m_running = false; }

    /// Save the current HDR image as a scene-referred EXR (untonemapped,
    /// chromaticities-tagged). The matching tonemapped PNG is written by the
    /// offscreen path only.
    bool saveExr(const std::filesystem::path& path);

  private:
    struct FrameResources {
        VkCommandBuffer renderCmd{};  ///< scene-referred renderer recording
        VkCommandBuffer displayCmd{}; ///< tonemap recording (interactive only)
        VkSemaphore imageAvailable{};
        uint64_t completionValue{}; ///< highest timeline value signalled for this slot
    };

    [[nodiscard]] bool bootstrap();
    void shutdown() noexcept;
    int mainLoop();
    [[nodiscard]] int renderOffscreen();
    /// Record + submit the renderer into the HDR image; returns the timeline
    /// value signalled on completion and advances the frame slot.
    uint64_t renderSceneReferred();
    /// Acquire, tonemap, present the given completed scene-referred frame.
    void presentFrame(uint32_t slot, uint64_t renderValue);
    void handleResize(uint32_t w, uint32_t h);
    [[nodiscard]] bool createHdrImage();
    [[nodiscard]] bool createToneMapper();
    [[nodiscard]] std::filesystem::path resolveScenePath(const std::filesystem::path& sceneFile) const;

    Config m_config{};
    SDL_Window* m_window{};
    Context m_context{};
    CommandPool m_commandPool{};
    Swapchain m_swapchain{};
    Descriptors m_descriptors{};
    ToneMapper m_toneMapper{};
    Image m_hdrImage{};
    std::optional<IblProbe> m_iblProbe;

    ColorSpace::WorkingColorSpace m_workingColorSpace = ColorSpace::WorkingColorSpace::LinRec2020;
    uint32_t m_tonemapper = 0;

    std::array<FrameResources, 2> m_frames{};
    /// One binary semaphore per swapchain image: signalled by the display
    /// submit, consumed by vkQueuePresentKHR (indexed by imageIndex).
    std::vector<VkSemaphore> m_renderComplete;
    VkSemaphore m_timelineSemaphore{};
    uint64_t m_nextTimelineValue = 1;
    uint32_t m_currentFrame = 0;
    uint32_t m_frameIndex = 0;
    std::vector<VkImageLayout> m_swapchainLayouts;
    bool m_running = false;
};

} // namespace harmonia
