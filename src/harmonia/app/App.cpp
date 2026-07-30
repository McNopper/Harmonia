#include "harmonia/app/App.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "harmonia/core/Barrier.hpp"
#include "harmonia/core/Logger.hpp"
#include "harmonia/core/OneShot.hpp"
#include "harmonia/pipeline/AccumulationPass.hpp"
#include "harmonia/pipeline/PassContext.hpp"
#include "harmonia/pipeline/SceneOutputCopyPass.hpp"
#include "harmonia/presentation/ImageCapture.hpp"
#include "harmonia/utils/ColorSpace.hpp"

namespace harmonia {

namespace {

constexpr std::uint64_t kWaitForever = UINT64_MAX;

[[nodiscard]] VkResult createBinarySemaphore(VkDevice device, VkSemaphore& semaphore) {
    const VkSemaphoreCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
    };
    return vkCreateSemaphore(device, &info, nullptr, &semaphore);
}

[[nodiscard]] VkResult createTimelineSemaphore(VkDevice device, VkSemaphore& semaphore) {
    const VkSemaphoreTypeCreateInfo typeInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext = nullptr,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    const VkSemaphoreCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &typeInfo,
        .flags = 0,
    };
    return vkCreateSemaphore(device, &info, nullptr, &semaphore);
}

/// Drawable size in pixels (high-DPI aware) — the swapchain extent source.
[[nodiscard]] VkExtent2D windowPixelExtent(SDL_Window* window) {
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(window, &width, &height);
    return VkExtent2D{
        .width = static_cast<std::uint32_t>(std::max(width, 1)),
        .height = static_cast<std::uint32_t>(std::max(height, 1)),
    };
}

constexpr std::uint64_t kHashSeed = 1469598103934665603ULL;
constexpr std::uint64_t kHashPrime = 1099511628211ULL;

void hashCombineU64(std::uint64_t& hash, std::uint64_t value) noexcept {
    hash ^= value;
    hash *= kHashPrime;
}

class ToneMapStagePass final : public IRenderPass {
  public:
    explicit ToneMapStagePass(const ToneMapper* toneMapper) : m_toneMapper(toneMapper) {}

    void record(const PassContext& ctx) noexcept override {
        if (m_toneMapper == nullptr || !m_toneMapper->isValid() || ctx.swapchainView == VK_NULL_HANDLE) {
            return;
        }
        const Image* input = ctx.denoised != nullptr ? ctx.denoised : ctx.hdrBuffer;
        if (input == nullptr) {
            return;
        }
        m_toneMapper->record(ctx.cmd,
                             input->view(),
                             ctx.swapchainView,
                             ctx.extent,
                             ctx.colorSpace,
                             ctx.tonemapper,
                             ctx.workingColorSpace);
    }

    void onResize(VkExtent2D extent) noexcept override { m_extent = extent; }
    [[nodiscard]] const char* name() const noexcept override { return "ToneMapStagePass"; }

  private:
    const ToneMapper* m_toneMapper = nullptr;
    VkExtent2D m_extent{};
};

[[nodiscard]] std::string stageList(const std::vector<std::unique_ptr<IRenderPass>>& stages) {
    if (stages.empty()) {
        return "none";
    }
    std::ostringstream out;
    for (std::size_t i = 0; i < stages.size(); ++i) {
        if (i > 0) {
            out << " -> ";
        }
        out << stages[i]->name();
    }
    return out.str();
}

} // namespace

App::~App() {
    shutdown();
}

int App::run(Config&& config) {
    m_config = std::move(config);
    m_defaultStages = m_config.stages;

    if (!bootstrap()) {
        return 1;
    }
    if (!onInitialize()) {
        Logger::error("Renderer initialization failed");
        return 1;
    }
    Logger::info("Renderer: {}", renderer().name());

    if (!loadScene(m_config.sceneFile)) {
        return 1;
    }

    if (!m_config.outputFile.empty()) {
        return renderOffscreen();
    }

    m_running = true;
    return mainLoop();
}

bool App::bootstrap() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        Logger::error("SDL_Init failed: {}", SDL_GetError());
        return false;
    }

    const bool offscreen = !m_config.outputFile.empty();
    m_window =
        SDL_CreateWindow(m_config.title.c_str(),
                         static_cast<int>(m_config.width),
                         static_cast<int>(m_config.height),
                         SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                             (m_config.resizable ? SDL_WINDOW_RESIZABLE : 0U) | (offscreen ? SDL_WINDOW_HIDDEN : 0U));
    if (m_window == nullptr) {
        Logger::error("SDL_CreateWindow failed: {}", SDL_GetError());
        return false;
    }

    auto context = Context::create(Context::Config{
        .appName = m_config.title,
        .appVersion = VK_MAKE_VERSION(1, 0, 0),
        .enableValidation = m_config.validation,
        .window = m_window,
    });
    if (!context) {
        Logger::error("Context creation failed: VkResult {}", static_cast<int>(context.error()));
        return false;
    }
    m_context = std::move(*context);

    auto pool = CommandPool::create(m_context.deviceContext(), m_context.physicalDeviceInfo().graphicsFamily);
    if (!pool) {
        Logger::error("CommandPool creation failed: VkResult {}", static_cast<int>(pool.error()));
        return false;
    }
    m_commandPool = std::move(*pool);

    auto swapchain = Swapchain::create(m_context.deviceContext(), m_context.surface(), windowPixelExtent(m_window));
    if (!swapchain) {
        Logger::error("Swapchain creation failed: VkResult {}", static_cast<int>(swapchain.error()));
        return false;
    }
    m_swapchain = std::move(*swapchain);
    m_swapchainLayouts.assign(m_swapchain.imageCount(), VK_IMAGE_LAYOUT_UNDEFINED);

    auto descriptors = Descriptors::create(m_context.deviceContext());
    if (!descriptors) {
        Logger::error("Descriptor creation failed: VkResult {}", static_cast<int>(descriptors.error()));
        return false;
    }
    m_descriptors = std::move(*descriptors);

    if (!createHdrImage() || !createDenoisedImage() || !createToneMapper()) {
        return false;
    }
    rebuildStagePipeline();
    if (!m_hdrImage.isValid()) {
        return false;
    }

    const VkDevice device = m_context.deviceContext().device;
    for (FrameResources& frame : m_frames) {
        auto renderCmd = m_commandPool.allocate();
        auto displayCmd = m_commandPool.allocate();
        if (!renderCmd || !displayCmd) {
            Logger::error("Command buffer allocation failed");
            return false;
        }
        frame.renderCmd = *renderCmd;
        frame.displayCmd = *displayCmd;
        VkSemaphore imageSem{};
        if (createBinarySemaphore(device, imageSem) != VK_SUCCESS) {
            Logger::error("Semaphore creation failed");
            return false;
        }
        frame.imageAvailable = harmonia::UniqueSemaphore{device, imageSem};
    }
    m_renderComplete.reserve(m_swapchain.imageCount());
    for (std::uint32_t i = 0; i < m_swapchain.imageCount(); ++i) {
        VkSemaphore sem{};
        if (createBinarySemaphore(device, sem) != VK_SUCCESS) {
            Logger::error("renderComplete semaphore creation failed");
            return false;
        }
        m_renderComplete.emplace_back(device, sem);
    }
    {
        VkSemaphore timelineSem{};
        if (createTimelineSemaphore(device, timelineSem) != VK_SUCCESS) {
            Logger::error("Timeline semaphore creation failed");
            return false;
        }
        m_timelineSemaphore = harmonia::UniqueSemaphore{device, timelineSem};
    }

    return true;
}

bool App::createHdrImage() {
    // Usage covers both renderer families: storage writes (ray/compute) and
    // color-attachment + sampled reads (raster), plus read-back for capture.
    auto hdrImage =
        Image::create(m_context.deviceContext(),
                      m_swapchain.extent(),
                      VK_FORMAT_R32G32B32A32_SFLOAT,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT,
                      "harmonia.hdr");
    if (!hdrImage) {
        Logger::error("HDR image creation failed: VkResult {}", static_cast<int>(hdrImage.error()));
        return false;
    }
    m_hdrImage = std::move(*hdrImage);
    return true;
}

bool App::createDenoisedImage() {
    if (!m_config.stages.denoiser) {
        m_denoisedImage = {};
        return true;
    }

    // Scene-output buffer for the shared denoiser stage.
    auto denoisedImage =
        Image::create(m_context.deviceContext(),
                      m_swapchain.extent(),
                      VK_FORMAT_R32G32B32A32_SFLOAT,
                      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT,
                      "harmonia.denoised");
    if (!denoisedImage) {
        Logger::warn("Denoised image creation failed: VkResult {} — disabling denoiser stage",
                     static_cast<int>(denoisedImage.error()));
        m_denoisedImage = {};
        return true;
    }
    m_denoisedImage = std::move(*denoisedImage);
    return true;
}

bool App::createToneMapper() {
    if (!m_config.stages.tonemapper) {
        m_toneMapper = {};
        return true;
    }

    const std::filesystem::path shaderDir = HARMONIA_SHADER_DIR;
    auto toneMapper = ToneMapper::create(
        m_context.deviceContext(), m_swapchain.format(), shaderDir / "tonemap_vert.spv", shaderDir / "tonemap.spv");
    if (!toneMapper) {
        Logger::warn("Tone mapper creation failed: VkResult {} — disabling tonemapper stage",
                     static_cast<int>(toneMapper.error()));
        m_toneMapper = {};
        return true;
    }
    m_toneMapper = std::move(*toneMapper);
    return true;
}

void App::rebuildStagePipeline() {
    m_sceneStages.clear();
    m_displayStages.clear();
    m_sceneOutputUsesDenoised = false;
    ++m_stageEpoch;

    if (m_config.stages.accumulation) {
        const std::filesystem::path shaderPath = std::filesystem::path(HARMONIA_SHADER_DIR) / "accumulation.spv";
        auto accumulationPass = AccumulationPass::create(m_context.deviceContext(), m_swapchain.extent(), shaderPath);
        if (accumulationPass) {
            m_sceneStages.push_back(std::make_unique<AccumulationPass>(std::move(*accumulationPass)));
        } else {
            Logger::warn("Accumulation stage unavailable: VkResult {} — disabling accumulation stage",
                         static_cast<int>(accumulationPass.error()));
        }
    }

    if (m_config.stages.denoiser && m_denoisedImage.isValid()) {
        const std::filesystem::path shaderPath = std::filesystem::path(HARMONIA_SHADER_DIR) / "denoiser.spv";
        auto denoiserPass = SceneOutputCopyPass::create(m_context.deviceContext(),
                                                        m_swapchain.extent(),
                                                        shaderPath,
                                                        SceneOutputCopyPass::Settings{
                                                            .strength = m_config.denoiser.strength,
                                                            .iterations = m_config.denoiser.iterations,
                                                            .useHistory = m_config.denoiser.useHistory,
                                                            .historyBlend = m_config.denoiser.historyBlend,
                                                            .useGradient = m_config.denoiser.useGradient,
                                                            .gradientAlpha = m_config.denoiser.gradientAlpha,
                                                        });
        if (denoiserPass) {
            m_sceneStages.push_back(std::make_unique<SceneOutputCopyPass>(std::move(*denoiserPass)));
            m_sceneOutputUsesDenoised = true;
        } else {
            Logger::warn("Denoiser stage unavailable: VkResult {} — falling back to HDR scene output",
                         static_cast<int>(denoiserPass.error()));
        }
    } else if (m_config.stages.denoiser) {
        Logger::warn("Denoiser stage enabled but unavailable; falling back to HDR scene output");
    }

    if (m_config.stages.tonemapper && m_toneMapper.isValid()) {
        m_displayStages.push_back(std::make_unique<ToneMapStagePass>(&m_toneMapper));
    } else if (m_config.stages.tonemapper) {
        Logger::warn("Tonemapper stage enabled but unavailable; display tonemapping disabled");
    }

    for (const auto& pass : m_sceneStages) {
        pass->onResize(m_swapchain.extent());
    }
    for (const auto& pass : m_displayStages) {
        pass->onResize(m_swapchain.extent());
    }

    Logger::info("Scene stages: {}", stageList(m_sceneStages));
    Logger::info("Display stages: {}", stageList(m_displayStages));
}

void App::applySceneStageConfig(const SceneLoader::SceneConfig& sceneConfig) {
    m_config.stages = m_defaultStages;
    if (sceneConfig.accumulationStageEnabled.has_value()) {
        m_config.stages.accumulation = *sceneConfig.accumulationStageEnabled;
    }
    if (sceneConfig.denoiserStageEnabled.has_value()) {
        m_config.stages.denoiser = *sceneConfig.denoiserStageEnabled;
    }
    if (sceneConfig.tonemapperStageEnabled.has_value()) {
        m_config.stages.tonemapper = *sceneConfig.tonemapperStageEnabled;
    }
}

bool App::hasTonemapStage() const noexcept {
    return !m_displayStages.empty();
}

const Image& App::sceneOutputImage() const noexcept {
    return m_sceneOutputUsesDenoised ? m_denoisedImage : m_hdrImage;
}

VkPipelineStageFlags2 App::sceneOutputStageMask() noexcept {
    return m_sceneOutputUsesDenoised ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : renderer().outputStageMask();
}

VkAccessFlags2 App::sceneOutputAccessMask() noexcept {
    return m_sceneOutputUsesDenoised ? VK_ACCESS_2_SHADER_WRITE_BIT : renderer().outputAccessMask();
}

VkImageView App::denoiserGradientImageView() const noexcept {
    for (const auto& stage : m_sceneStages) {
        if (auto* pass = dynamic_cast<const SceneOutputCopyPass*>(stage.get())) {
            return pass->gradientImageView();
        }
    }
    return VK_NULL_HANDLE;
}

std::uint64_t App::accumulationResetToken() const noexcept {
    std::uint64_t token = kHashSeed;
    hashCombineU64(token, m_sceneEpoch);
    hashCombineU64(token, m_extentEpoch);
    hashCombineU64(token, m_stageEpoch);
    hashCombineU64(token, static_cast<std::uint64_t>(m_workingColorSpace));
    hashCombineU64(token, static_cast<std::uint64_t>(m_tonemapper));
    hashCombineU64(token, m_config.stages.accumulation ? 1ULL : 0ULL);
    hashCombineU64(token, m_config.stages.denoiser ? 1ULL : 0ULL);
    hashCombineU64(token, m_config.stages.tonemapper ? 1ULL : 0ULL);
    hashCombineU64(token, std::bit_cast<std::uint32_t>(m_config.denoiser.strength));
    hashCombineU64(token, m_config.denoiser.iterations);
    hashCombineU64(token, m_config.denoiser.useHistory ? 1ULL : 0ULL);
    hashCombineU64(token, std::bit_cast<std::uint32_t>(m_config.denoiser.historyBlend));
    hashCombineU64(token, m_config.denoiser.useGradient ? 1ULL : 0ULL);
    hashCombineU64(token, std::bit_cast<std::uint32_t>(m_config.denoiser.gradientAlpha));
    // While accumulating (offscreen capture or interactive progressive mode) the
    // token must stay constant across frames so history builds up; it changes
    // only when the view/scene changes (m_accumViewEpoch, bumped by
    // resetAccumulation()). When interactive accumulation is OFF, fold in the
    // per-frame index so every interactive frame is a fresh, non-accumulated sample.
    if (m_config.outputFile.empty() && !m_interactiveAccumulation) {
        hashCombineU64(token, static_cast<std::uint64_t>(m_frameIndex));
    } else {
        hashCombineU64(token, m_accumViewEpoch);
    }
    return token;
}

std::uint64_t App::denoiserResetToken() const noexcept {
    // Hashes only scene/extent/config changes — NOT m_accumViewEpoch (camera movement).
    // This keeps the A-SVGF denoiser's temporal history intact across camera motion so
    // it can reproject via motion vectors (A-SVGF / SVGF design: Schied et al. 2017/2018).
    std::uint64_t token = kHashSeed;
    hashCombineU64(token, m_sceneEpoch);
    hashCombineU64(token, m_extentEpoch);
    hashCombineU64(token, m_stageEpoch);
    hashCombineU64(token, static_cast<std::uint64_t>(m_workingColorSpace));
    hashCombineU64(token, m_config.stages.denoiser ? 1ULL : 0ULL);
    hashCombineU64(token, std::bit_cast<std::uint32_t>(m_config.denoiser.strength));
    hashCombineU64(token, m_config.denoiser.iterations);
    hashCombineU64(token, m_config.denoiser.useHistory ? 1ULL : 0ULL);
    hashCombineU64(token, std::bit_cast<std::uint32_t>(m_config.denoiser.historyBlend));
    hashCombineU64(token, m_config.denoiser.useGradient ? 1ULL : 0ULL);
    hashCombineU64(token, std::bit_cast<std::uint32_t>(m_config.denoiser.gradientAlpha));
    return token;
}

std::filesystem::path App::resolveScenePath(const std::filesystem::path& sceneFile) const {
    // Absolute/existing path is used as-is; otherwise the bare name (with an
    // optional ".scene.toml" extension) is looked up in the assets directory —
    // the canonical Aether asset collection.
    std::filesystem::path resolved =
        sceneFile.empty() ? std::filesystem::path("cornell_classic.scene.toml") : sceneFile;
    std::error_code ec;
    if (!std::filesystem::exists(resolved, ec)) {
        std::filesystem::path candidate = m_config.assetsDir / resolved.filename();
        if (!candidate.string().ends_with(".scene.toml")) {
            candidate += ".scene.toml";
        }
        resolved = candidate;
    }
    return resolved;
}

bool App::loadScene(const std::filesystem::path& sceneFile) {
    const std::filesystem::path resolved = resolveScenePath(sceneFile);

    vkDeviceWaitIdle(m_context.deviceContext().device);
    m_iblProbe.reset();
    onSceneUnload();

    // Aether scene data flows into the renderer through one interface only:
    // the renderer's ISceneBuilder — identical for Harmonia, Hyperion, Theia.
    SceneLoader loader;
    const auto sceneConfig =
        loader.load(resolved, m_config.assetsDir, sceneBuilder(), m_context.deviceContext(), m_commandPool);
    if (!sceneConfig) {
        Logger::error("Scene load failed: {}", resolved.string());
        return false;
    }

    m_workingColorSpace = sceneConfig->workingColorSpace;
    m_tonemapper = sceneConfig->tonemapper.value_or(0U);
    applySceneStageConfig(*sceneConfig);
    Logger::info("Stage config: accumulation={}, denoiser={}, tonemapper={}",
                 m_config.stages.accumulation,
                 m_config.stages.denoiser,
                 m_config.stages.tonemapper);
    Logger::info("Denoiser config: strength={:.3f}, iterations={}, history={}, history_blend={:.3f}, gradient={}, "
                 "gradient_alpha={:.3f}",
                 m_config.denoiser.strength,
                 m_config.denoiser.iterations,
                 m_config.denoiser.useHistory,
                 m_config.denoiser.historyBlend,
                 m_config.denoiser.useGradient,
                 m_config.denoiser.gradientAlpha);
    if (sceneConfig->postTonemapRenderer) {
        if (*sceneConfig->postTonemapRenderer == "green_screen") {
            m_config.displayOverlay = true;
        } else {
            Logger::warn("Unsupported post-tonemap renderer '{}' — ignoring", *sceneConfig->postTonemapRenderer);
        }
    }
    Logger::info("Working color space: {}", ColorSpace::interopId(m_workingColorSpace));

    // IBL environment map (shared descriptor wiring lives here; renderers
    // consume the probe via iblProbe()).
    if (sceneConfig->envMapFile) {
        const auto envPath = m_config.assetsDir / *sceneConfig->envMapFile;
        auto probe = IblProbe::loadFromEXR(m_context.deviceContext(), m_commandPool, envPath, m_workingColorSpace);
        if (!probe) {
            Logger::warn("IBL probe load failed for '{}' — using procedural sky", envPath.string());
        } else {
            m_iblProbe = std::move(*probe);
            if (const VkResult result = m_descriptors.updateEnvMap(
                    m_context.deviceContext(), m_iblProbe->imageView(), m_iblProbe->sampler());
                result != VK_SUCCESS) {
                Logger::warn("IBL descriptor update failed: VkResult {}", static_cast<int>(result));
            } else {
                Logger::info("IBL probe loaded: '{}'", envPath.filename().string());
            }
            if (m_iblProbe->cdfWidth() > 0) {
                if (const VkResult result =
                        m_descriptors.updateEnvImportance(m_context.deviceContext(),
                                                          m_iblProbe->marginalCdfBuffer().handle(),
                                                          m_iblProbe->conditionalCdfBuffer().handle());
                    result != VK_SUCCESS) {
                    Logger::warn("IBL importance descriptor update failed: VkResult {}", static_cast<int>(result));
                } else {
                    Logger::info("IBL importance CDF descriptors updated ({}×{})",
                                 m_iblProbe->cdfWidth(),
                                 m_iblProbe->cdfHeight());
                }
            }
        }
    }

    if (!onSceneLoaded(*sceneConfig)) {
        Logger::error("Renderer scene setup failed: {}", resolved.string());
        return false;
    }

    if (!createDenoisedImage() || !createToneMapper()) {
        return false;
    }
    ++m_sceneEpoch;
    rebuildStagePipeline();

    return true;
}

std::uint64_t App::renderSceneReferred() {
    FrameResources& frame = m_frames[m_currentFrame];

    // Wait for the previous use of this frame slot to complete.
    if (frame.completionValue > 0U) {
        const VkSemaphoreWaitInfo waitInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .pNext = nullptr,
            .flags = 0,
            .semaphoreCount = 1,
            .pSemaphores = m_timelineSemaphore.ptr(),
            .pValues = &frame.completionValue,
        };
        vkWaitSemaphores(m_context.deviceContext().device, &waitInfo, kWaitForever);
    }

    vkResetCommandBuffer(frame.renderCmd, 0);
    const VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    if (vkBeginCommandBuffer(frame.renderCmd, &beginInfo) != VK_SUCCESS) {
        Logger::error("Failed to begin render command buffer");
        return frame.completionValue;
    }

    // The renderer produces a linear image in the scene-referred working
    // color space and leaves it in VK_IMAGE_LAYOUT_GENERAL.
    const RenderTarget target{
        .image = m_hdrImage.handle(),
        .view = m_hdrImage.view(),
        .extent = m_hdrImage.extent(),
        .colorSpace = m_swapchain.outputColorSpace(),
        .workingColorSpace = m_workingColorSpace,
    };
    renderer().record(frame.renderCmd, target);

    // Allow subclasses to split the frame for async compute.
    // Default: {frame.renderCmd, VK_NULL_HANDLE} — single-queue path unchanged.
    auto [stagesCmd, asyncWaitSem] = onBeforeSceneStages(frame.renderCmd);

    const PassContext passContext{
        .cmd = stagesCmd,
        .frameIndex = m_frameIndex,
        .frameSampleIndex = m_frameIndex,
        .rngSeed = m_config.rngSeed,
        .deterministicReplay = m_config.deterministicReplay,
        .extent = m_hdrImage.extent(),
        .fixedView = !m_config.outputFile.empty() || m_interactiveAccumulation,
        .accumulationResetToken = accumulationResetToken(),
        .denoiserResetToken = denoiserResetToken(),
        .hdrBuffer = &m_hdrImage,
        .gNormalView = renderer().gNormalView(),
        .gDepthView = renderer().gDepthView(),
        .motionVectorView = renderer().motionVectorView(),
        .denoised = m_denoisedImage.isValid() ? &m_denoisedImage : nullptr,
        .swapchainView = VK_NULL_HANDLE,
        .colorSpace = m_swapchain.outputColorSpace(),
        .tonemapper = m_tonemapper,
        .workingColorSpace = m_workingColorSpace,
    };
    for (const auto& pass : m_sceneStages) {
        if (pass) {
            pass->record(passContext);
        }
    }

    if (vkEndCommandBuffer(stagesCmd) != VK_SUCCESS) {
        Logger::error("Failed to end render command buffer");
        return frame.completionValue;
    }

    const std::uint64_t signalValue = m_nextTimelineValue++;
    const VkSemaphoreSubmitInfo timelineSignal{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .pNext = nullptr,
        .semaphore = m_timelineSemaphore,
        .value = signalValue,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .deviceIndex = 0,
    };
    const VkSemaphoreSubmitInfo asyncWait{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .pNext = nullptr,
        .semaphore = asyncWaitSem,
        .value = 0,
        .stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .deviceIndex = 0,
    };
    const std::span<const VkSemaphoreSubmitInfo> waitSemaphores =
        (asyncWaitSem != VK_NULL_HANDLE) ? std::span<const VkSemaphoreSubmitInfo>{&asyncWait, 1}
                                         : std::span<const VkSemaphoreSubmitInfo>{};
    if (submitOneShot(m_context.deviceContext().graphicsQueue,
                      stagesCmd,
                      VK_NULL_HANDLE,
                      waitSemaphores,
                      std::span<const VkSemaphoreSubmitInfo>{&timelineSignal, 1}) != VK_SUCCESS) {
        Logger::error("Render queue submit failed");
        return frame.completionValue;
    }

    frame.completionValue = signalValue;
    ++m_frameIndex;
    m_currentFrame = (m_currentFrame + 1U) % static_cast<std::uint32_t>(m_frames.size());
    return signalValue;
}

void App::presentFrame(std::uint32_t slot, std::uint64_t renderValue) {
    auto imageIndex = acquireFrame(slot);
    if (!imageIndex) {
        return;
    }

    FrameResources& frame = m_frames[slot];

    recordDisplayBarriers(frame.displayCmd, *imageIndex);

    const PassContext displayPassContext{
        .cmd = frame.displayCmd,
        .frameIndex = m_frameIndex,
        .frameSampleIndex = m_frameIndex,
        .rngSeed = m_config.rngSeed,
        .deterministicReplay = m_config.deterministicReplay,
        .extent = m_swapchain.extent(),
        .hdrBuffer = &m_hdrImage,
        .gNormalView = VK_NULL_HANDLE,
        .gDepthView = VK_NULL_HANDLE,
        .denoised = m_sceneOutputUsesDenoised ? &m_denoisedImage : nullptr,
        .swapchainView = m_swapchain.imageView(*imageIndex),
        .colorSpace = m_swapchain.outputColorSpace(),
        .tonemapper = m_tonemapper,
        .workingColorSpace = m_workingColorSpace,
    };
    for (const auto& pass : m_displayStages) {
        if (pass) {
            pass->record(displayPassContext);
        }
    }

    bool swapchainInGeneral = false;
    if (!hasTonemapStage() && !m_config.displayOverlay) {
        const std::array toGeneral{
            imageBarrier(m_swapchain.image(*imageIndex),
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT),
        };
        pipelineBarrier(frame.displayCmd, toGeneral);
        const VkClearColorValue clear{
            .float32 = {0.0F, 0.0F, 0.0F, 1.0F},
        };
        const VkImageSubresourceRange range{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        };
        vkCmdClearColorImage(
            frame.displayCmd, m_swapchain.image(*imageIndex), VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
        swapchainInGeneral = true;
    }

    if (m_config.displayOverlay) {
        if (!m_displayOverlayLogged) {
            Logger::info("Display overlay renderer enabled");
            m_displayOverlayLogged = true;
        }
        const std::array displayPreBarriers{
            imageBarrier(m_swapchain.image(*imageIndex),
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT),
        };
        pipelineBarrier(frame.displayCmd, displayPreBarriers);
        swapchainInGeneral = true;

        const RenderTarget displayTarget{
            .image = m_swapchain.image(*imageIndex),
            .view = m_swapchain.imageView(*imageIndex),
            .extent = m_swapchain.extent(),
            .colorSpace = m_swapchain.outputColorSpace(),
            .workingColorSpace = m_workingColorSpace,
        };
        m_displayRenderer.record(frame.displayCmd, displayTarget);
    }

    const std::array presentBarrier{
        imageBarrier(m_swapchain.image(*imageIndex),
                     swapchainInGeneral ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     swapchainInGeneral ? VK_PIPELINE_STAGE_2_TRANSFER_BIT
                                        : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     swapchainInGeneral ? VK_ACCESS_2_TRANSFER_WRITE_BIT : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_NONE,
                     0),
    };
    pipelineBarrier(frame.displayCmd, presentBarrier);

    if (submitDisplay(slot, *imageIndex, renderValue) != VK_SUCCESS) {
        return;
    }

    presentAndHandleResize(*imageIndex);
}

std::optional<std::uint32_t> App::acquireFrame(std::uint32_t slot) noexcept {
    FrameResources& frame = m_frames[slot];

    std::uint32_t imageIndex = 0;
    VkResult result = m_swapchain.acquireNextImage(frame.imageAvailable, imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        const VkExtent2D extent = windowPixelExtent(m_window);
        handleResize(extent.width, extent.height);
        return std::nullopt;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        Logger::error("Swapchain acquire failed: VkResult {}", static_cast<int>(result));
        return std::nullopt;
    }

    vkResetCommandBuffer(frame.displayCmd, 0);
    const VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    if (vkBeginCommandBuffer(frame.displayCmd, &beginInfo) != VK_SUCCESS) {
        Logger::error("Failed to begin display command buffer");
        return std::nullopt;
    }
    return imageIndex;
}

void App::recordDisplayBarriers(VkCommandBuffer cmd, std::uint32_t imageIndex) noexcept {
    const std::array preToneMapBarriers{
        imageBarrier(sceneOutputImage().handle(),
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     sceneOutputStageMask(),
                     sceneOutputAccessMask(),
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT),
        imageBarrier(m_swapchain.image(imageIndex),
                     m_swapchainLayouts[imageIndex],
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_NONE,
                     0,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT),
    };
    pipelineBarrier(cmd, preToneMapBarriers);
}

VkResult App::submitDisplay(std::uint32_t slot, std::uint32_t imageIndex, std::uint64_t renderValue) noexcept {
    FrameResources& frame = m_frames[slot];

    if (vkEndCommandBuffer(frame.displayCmd) != VK_SUCCESS) {
        Logger::error("Failed to end display command buffer");
        return VK_ERROR_UNKNOWN;
    }

    const std::uint64_t displayValue = m_nextTimelineValue++;
    const VkPipelineStageFlags2 sceneReadyWaitStage =
        hasTonemapStage() ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    const VkPipelineStageFlags2 imageAcquireWaitStage =
        hasTonemapStage() ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    const std::array<VkSemaphoreSubmitInfo, 2> waitInfos{{
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = m_timelineSemaphore,
            .value = renderValue,
            .stageMask = sceneReadyWaitStage,
            .deviceIndex = 0,
        },
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = frame.imageAvailable,
            .value = 0,
            .stageMask = imageAcquireWaitStage,
            .deviceIndex = 0,
        },
    }};
    const std::array<VkSemaphoreSubmitInfo, 2> signalInfos{{
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = m_renderComplete[imageIndex],
            .value = 0,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .deviceIndex = 0,
        },
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = m_timelineSemaphore,
            .value = displayValue,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .deviceIndex = 0,
        },
    }};
    const VkResult result = submitOneShot(
        m_context.deviceContext().graphicsQueue, frame.displayCmd, VK_NULL_HANDLE, waitInfos, signalInfos);
    if (result != VK_SUCCESS) {
        Logger::error("Display submit failed: VkResult {}", static_cast<int>(result));
        return result;
    }

    // This slot must not be reused until display has also completed.
    frame.completionValue = displayValue;
    return VK_SUCCESS;
}

void App::presentAndHandleResize(std::uint32_t imageIndex) noexcept {
    const VkResult result =
        m_swapchain.present(m_context.deviceContext().graphicsQueue, imageIndex, m_renderComplete[imageIndex]);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        m_swapchainLayouts[imageIndex] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        const VkExtent2D extent = windowPixelExtent(m_window);
        handleResize(extent.width, extent.height);
        return;
    }
    if (result != VK_SUCCESS) {
        Logger::error("Present failed: VkResult {}", static_cast<int>(result));
    }
    m_swapchainLayouts[imageIndex] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
}

int App::mainLoop() {
    Logger::info("Interactive render loop");
    std::uint64_t lastTick = SDL_GetTicksNS();

    while (m_running) {
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            if (onEvent(event)) {
                continue;
            }
            if (event.type == SDL_EVENT_QUIT || (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)) {
                m_running = false;
            } else if (event.type == SDL_EVENT_WINDOW_RESIZED || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                const VkExtent2D extent = windowPixelExtent(m_window);
                handleResize(extent.width, extent.height);
            }
        }

        const std::uint64_t now = SDL_GetTicksNS();
        const float dtSeconds = static_cast<float>(now - lastTick) * 1e-9F;
        lastTick = now;
        onUpdate(dtSeconds);

        if (!m_running || m_swapchain.extent().width == 0U || m_swapchain.extent().height == 0U) {
            continue;
        }

        // Save the frame slot before renderSceneReferred advances it.
        const std::uint32_t slot = m_currentFrame;
        const std::uint64_t renderValue = renderSceneReferred();
        presentFrame(slot, renderValue);
    }

    vkDeviceWaitIdle(m_context.deviceContext().device);
    return 0;
}

int App::renderOffscreen() {
    const std::uint32_t frameCount = offscreenFrameCount();
    Logger::info("Offscreen render: {} frame(s) -> {}", frameCount, m_config.outputFile.string());
    for (std::uint32_t i = 0; i < frameCount; ++i) {
        renderSceneReferred();
    }
    vkDeviceWaitIdle(m_context.deviceContext().device);

    const bool isPng = (m_config.outputFile.extension() == ".png");
    bool ok = true;

    // Scene-referred linear EXR (chromaticities-tagged, untonemapped) when not a bare .png.
    if (!isPng) {
        ok = ImageCapture::saveExr(
            m_context.deviceContext(), m_commandPool, sceneOutputImage(), m_config.outputFile, m_workingColorSpace);
    }

    // Tone-mapped PNG. To match the interactive window, run the SAME tone mapper operator
    // (m_tonemapper) into a fixed 8-bit sRGB capture image and read that back — instead of a
    // separate, hardcoded CPU tone map. Falls back to the CPU ACES path only when the tone
    // mapper stage is unavailable.
    std::filesystem::path pngPath = m_config.outputFile;
    if (!isPng) {
        pngPath.replace_extension(".png");
    }
    if (m_toneMapper.isValid() &&
        m_offscreen.tonemapToCaptureImage(deviceContext(),
                                          commandPool(),
                                          m_swapchain.extent(),
                                          sceneOutputImage(),
                                          sceneOutputStageMask(),
                                          sceneOutputAccessMask(),
                                          m_tonemapper,
                                          m_workingColorSpace)) {
        // Capture image is VK_FORMAT_R8G8B8A8_UNORM (RGBA) — no channel swap needed.
        ok = ImageCapture::saveSdrPng(
                 m_context.deviceContext(), m_commandPool, m_offscreen.captureImage(), pngPath, /*swapRB=*/false) &&
             ok;
    } else {
        ok = ImageCapture::savePng(
                 m_context.deviceContext(), m_commandPool, sceneOutputImage(), pngPath, m_workingColorSpace) &&
             ok;
    }
    return ok ? 0 : 1;
}

bool App::saveExr(const std::filesystem::path& path) {
    vkDeviceWaitIdle(m_context.deviceContext().device);
    return ImageCapture::saveExr(
        m_context.deviceContext(), m_commandPool, sceneOutputImage(), path, m_workingColorSpace);
}

void App::handleResize(std::uint32_t w, std::uint32_t h) {
    if (w == 0U || h == 0U || m_context.deviceContext().device == VK_NULL_HANDLE) {
        return;
    }

    vkDeviceWaitIdle(m_context.deviceContext().device);

    // Reset all frame command buffers to INITIAL state before destroying any
    // resources they may reference (descriptor sets, images). This prevents
    // the validation layer from flagging destroyed-while-still-referenced errors.
    for (FrameResources& frame : m_frames) {
        vkResetCommandBuffer(frame.renderCmd, 0);
        vkResetCommandBuffer(frame.displayCmd, 0);
        frame.completionValue = 0U;
    }
    // NOTE: m_nextTimelineValue is NOT reset — timeline semaphores require
    // monotonically increasing signal values. Frame completionValues are reset
    // to 0 so they are treated as "not in flight" after a resize.

    const VkExtent2D newExtent{
        .width = w,
        .height = h,
    };
    if (m_swapchain.recreate(newExtent) != VK_SUCCESS) {
        Logger::error("Swapchain recreate failed");
        return;
    }
    m_swapchainLayouts.assign(m_swapchain.imageCount(), VK_IMAGE_LAYOUT_UNDEFINED);

    // Recreate per-image renderComplete semaphores to match the new swapchain
    // image count. vkDeviceWaitIdle() above guarantees they are unsignaled.
    const VkDevice device = m_context.deviceContext().device;
    m_renderComplete.clear();
    m_renderComplete.reserve(m_swapchain.imageCount());
    for (std::uint32_t i = 0; i < m_swapchain.imageCount(); ++i) {
        VkSemaphore sem{};
        if (createBinarySemaphore(device, sem) != VK_SUCCESS) {
            Logger::error("renderComplete semaphore recreate failed");
            return;
        }
        m_renderComplete.emplace_back(device, sem);
    }

    if (!createHdrImage()) {
        return;
    }
    if (!createDenoisedImage()) {
        return;
    }

    // Recreate tone mapper in case the swapchain format changed (HDR10 ↔ SDR).
    if (!createToneMapper()) {
        return;
    }
    ++m_extentEpoch;
    rebuildStagePipeline();

    if (m_config.displayOverlay) {
        m_displayRenderer.onResize(m_swapchain.extent());
    }

    renderer().onResize(m_swapchain.extent());
    onResized(m_swapchain.extent());
}

void App::shutdown() noexcept {
    if (m_context.deviceContext().device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_context.deviceContext().device);
        for (FrameResources& frame : m_frames) {
            frame = {};
        }
        m_renderComplete.clear();
        m_timelineSemaphore.reset();
    }

    m_iblProbe.reset();
    m_sceneStages.clear();
    m_displayStages.clear();
    m_sceneOutputUsesDenoised = false;
    m_hdrImage = {};
    m_denoisedImage = {};
    m_offscreen = {};
    m_toneMapper = {};
    m_descriptors = {};
    m_swapchain = {};
    m_commandPool = {};
    m_context = {};

    if (m_window != nullptr) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
        SDL_Quit();
    }
}

} // namespace harmonia
