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
#include <thread>
#include <utility>

#include "harmonia/app/AppDetail.hpp"
#include "harmonia/core/Barrier.hpp"
#include "harmonia/core/Logger.hpp"
#include "harmonia/core/OneShot.hpp"
#include "harmonia/core/ProcessPriority.hpp"
#include "harmonia/pipeline/AccumulationPass.hpp"
#include "harmonia/pipeline/PassContext.hpp"
#include "harmonia/pipeline/SceneOutputCopyPass.hpp"
#include "harmonia/presentation/ImageCapture.hpp"
#include "harmonia/utils/ColorSpace.hpp"

namespace harmonia::app {

constexpr std::uint64_t kHashSeed = 1469598103934665603ULL;
constexpr std::uint64_t kHashPrime = 1099511628211ULL;

void hashCombineU64(std::uint64_t& hash, std::uint64_t value) noexcept {
    hash ^= value;
    hash *= kHashPrime;
}

class ToneMapStagePass final : public pipeline::IRenderPass {
  public:
    explicit ToneMapStagePass(const presentation::ToneMapper* toneMapper) : m_toneMapper(toneMapper) {}

    void record(const pipeline::PassContext& ctx) noexcept override {
        if (m_toneMapper == nullptr || !m_toneMapper->isValid() || ctx.swapchainView == VK_NULL_HANDLE) {
            return;
        }
        const core::Image* input = ctx.denoised != nullptr ? ctx.denoised : ctx.hdrBuffer;
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
    const presentation::ToneMapper* m_toneMapper = nullptr;
    VkExtent2D m_extent{};
};

[[nodiscard]] std::string stageList(const std::vector<std::unique_ptr<pipeline::IRenderPass>>& stages) {
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
        core::Logger::error("Renderer initialization failed");
        return 1;
    }
    core::Logger::info("Renderer: {}", renderer().name());

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
        core::Logger::error("SDL_Init failed: {}", SDL_GetError());
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
        core::Logger::error("SDL_CreateWindow failed: {}", SDL_GetError());
        return false;
    }

    auto context = vulkan_init::Context::create(vulkan_init::Context::Config{
        .appName = m_config.title,
        .appVersion = VK_MAKE_VERSION(1, 0, 0),
        .enableValidation = m_config.validation,
        .window = m_window,
    });
    if (!context) {
        core::Logger::error("vulkan_init::Context creation failed: VkResult {}", static_cast<int>(context.error()));
        return false;
    }
    m_context = std::move(*context);
    logGpuClockCalibration();

    auto pool = core::CommandPool::create(m_context.deviceContext(), m_context.physicalDeviceInfo().graphicsFamily);
    if (!pool) {
        core::Logger::error("core::CommandPool creation failed: VkResult {}", static_cast<int>(pool.error()));
        return false;
    }
    m_commandPool = std::move(*pool);

    auto swapchain =
        presentation::Swapchain::create(m_context.deviceContext(), m_context.surface(), windowPixelExtent(m_window));
    if (!swapchain) {
        core::Logger::error("presentation::Swapchain creation failed: VkResult {}",
                            static_cast<int>(swapchain.error()));
        return false;
    }
    m_swapchain = std::move(*swapchain);

    auto descriptors = renderer::Descriptors::create(m_context.deviceContext());
    if (!descriptors) {
        core::Logger::error("Descriptor creation failed: VkResult {}", static_cast<int>(descriptors.error()));
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

    if (!m_frameSync.create(m_context.deviceContext(), m_commandPool, m_swapchain.imageCount())) {
        return false;
    }

    return true;
}

void App::logGpuClockCalibration() noexcept {
    const auto& ctx = m_context.deviceContext();
    if (!ctx.calibratedTimestampsSupported) {
        return;
    }

    // VK_KHR_calibrated_timestamps: correlate the GPU timestamp clock with a host clock.
    std::uint32_t domainCount = 0;
    if (vkGetPhysicalDeviceCalibrateableTimeDomainsKHR(ctx.physicalDevice, &domainCount, nullptr) != VK_SUCCESS ||
        domainCount == 0U) {
        return;
    }
    std::vector<VkTimeDomainEXT> domains(domainCount);
    if (vkGetPhysicalDeviceCalibrateableTimeDomainsKHR(ctx.physicalDevice, &domainCount, domains.data()) !=
        VK_SUCCESS) {
        return;
    }

    VkTimeDomainEXT hostDomain = VK_TIME_DOMAIN_DEVICE_EXT;
    for (const VkTimeDomainEXT d : domains) {
        if (d == VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT || d == VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT) {
            hostDomain = d;
            break;
        }
    }
    if (hostDomain == VK_TIME_DOMAIN_DEVICE_EXT) {
        return; // no usable host clock domain advertised
    }

    const std::array<VkCalibratedTimestampInfoEXT, 2> infos{{
        {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr, VK_TIME_DOMAIN_DEVICE_EXT},
        {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr, hostDomain},
    }};
    std::array<std::uint64_t, 2> timestamps{0, 0};
    std::uint64_t maxDeviation = 0;
    if (vkGetCalibratedTimestampsKHR(
            ctx.device, static_cast<std::uint32_t>(infos.size()), infos.data(), timestamps.data(), &maxDeviation) !=
        VK_SUCCESS) {
        return;
    }

    const double periodNs = m_context.physicalDeviceInfo().properties.properties.limits.timestampPeriod;
    core::Logger::info("Calibrated GPU clock: {:.4f} ns/tick, GPU<->host max deviation {} ns", periodNs, maxDeviation);
}

bool App::createHdrImage() {
    // Usage covers both renderer families: storage writes (ray/compute) and
    // color-attachment + sampled reads (raster), plus read-back for capture.
    auto hdrImage = core::Image::create(m_context.deviceContext(),
                                        m_swapchain.extent(),
                                        VK_FORMAT_R32G32B32A32_SFLOAT,
                                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                            VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                        VK_IMAGE_ASPECT_COLOR_BIT,
                                        "harmonia.hdr");
    if (!hdrImage) {
        core::Logger::error("HDR image creation failed: VkResult {}", static_cast<int>(hdrImage.error()));
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
    auto denoisedImage = core::Image::create(m_context.deviceContext(),
                                             m_swapchain.extent(),
                                             VK_FORMAT_R32G32B32A32_SFLOAT,
                                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                             VK_IMAGE_ASPECT_COLOR_BIT,
                                             "harmonia.denoised");
    if (!denoisedImage) {
        core::Logger::warn("Denoised image creation failed: VkResult {} — disabling denoiser stage",
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
    auto toneMapper = presentation::ToneMapper::create(
        m_context.deviceContext(), m_swapchain.format(), shaderDir / "tonemap_vert.spv", shaderDir / "tonemap.spv");
    if (!toneMapper) {
        core::Logger::warn("Tone mapper creation failed: VkResult {} — disabling tonemapper stage",
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
        auto accumulationPass =
            pipeline::AccumulationPass::create(m_context.deviceContext(), m_swapchain.extent(), shaderPath);
        if (accumulationPass) {
            m_sceneStages.push_back(std::make_unique<pipeline::AccumulationPass>(std::move(*accumulationPass)));
        } else {
            core::Logger::warn("Accumulation stage unavailable: VkResult {} — disabling accumulation stage",
                               static_cast<int>(accumulationPass.error()));
        }
    }

    if (m_config.stages.denoiser && m_denoisedImage.isValid()) {
        const std::filesystem::path shaderPath = std::filesystem::path(HARMONIA_SHADER_DIR) / "denoiser.spv";
        auto denoiserPass = pipeline::SceneOutputCopyPass::create(m_context.deviceContext(),
                                                                  m_swapchain.extent(),
                                                                  shaderPath,
                                                                  pipeline::SceneOutputCopyPass::Settings{
                                                                      .strength = m_config.denoiser.strength,
                                                                      .iterations = m_config.denoiser.iterations,
                                                                      .useHistory = m_config.denoiser.useHistory,
                                                                      .historyBlend = m_config.denoiser.historyBlend,
                                                                      .useGradient = m_config.denoiser.useGradient,
                                                                      .gradientAlpha = m_config.denoiser.gradientAlpha,
                                                                  });
        if (denoiserPass) {
            m_sceneStages.push_back(std::make_unique<pipeline::SceneOutputCopyPass>(std::move(*denoiserPass)));
            m_sceneOutputUsesDenoised = true;
        } else {
            core::Logger::warn("Denoiser stage unavailable: VkResult {} — falling back to HDR scene output",
                               static_cast<int>(denoiserPass.error()));
        }
    } else if (m_config.stages.denoiser) {
        core::Logger::warn("Denoiser stage enabled but unavailable; falling back to HDR scene output");
    }

    if (m_config.stages.tonemapper && m_toneMapper.isValid()) {
        m_displayStages.push_back(std::make_unique<ToneMapStagePass>(&m_toneMapper));
    } else if (m_config.stages.tonemapper) {
        core::Logger::warn("Tonemapper stage enabled but unavailable; display tonemapping disabled");
    }

    for (const auto& pass : m_sceneStages) {
        pass->onResize(m_swapchain.extent());
    }
    for (const auto& pass : m_displayStages) {
        pass->onResize(m_swapchain.extent());
    }

    core::Logger::info("Scene stages: {}", stageList(m_sceneStages));
    core::Logger::info("Display stages: {}", stageList(m_displayStages));
}

void App::applySceneStageConfig(const scene::SceneLoader::SceneConfig& sceneConfig) {
    m_config.stages = m_defaultStages;
    // Two-tier output contract: the denoiser is an INTERACTIVE presentation filter only.
    // An offscreen capture (--output) is the scene-referred *estimator result* — the
    // artifact used for Hyperion/Theia parity and for reference screenshots — so the
    // denoiser is identity there by default, for BOTH renderers. --no-postfx requests
    // the same raw-estimator identity in the interactive window.
    //
    // Why this must be the default rather than an opt-out flag: the shared a-trous filter
    // has a FIXED PIXEL RADIUS, so the amount of image structure it destroys depends on
    // render resolution. A denoised render therefore does NOT converge to the true image
    // as samples/resolution increase (measured: Hyperion 320x240 disagreed with its own
    // 4x-supersampled result by 13.7/255 on background detail, and was spp-independent).
    // It is also applied ASYMMETRICALLY — Theia feeds it A-SVGF gradient/variance guidance
    // so detail survives, while Hyperion's single-frame offscreen render is smeared — which
    // made the "ground truth" the blurrier image and produced background-dependent parity
    // errors (env-lit regions off by 12-21/255 while flat geometry matched).
    // Explicit scene/render-preset config below still wins.
    if (!m_config.outputFile.empty() || m_config.noPostfx) {
        m_config.stages.denoiser = false;
    }
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

const core::Image& App::sceneOutputImage() const noexcept {
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
        if (auto* pass = dynamic_cast<const pipeline::SceneOutputCopyPass*>(stage.get())) {
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
    // the renderer's scene::ISceneBuilder — identical for Harmonia, Hyperion, Theia.
    scene::SceneLoader loader;
    const auto sceneConfig =
        loader.load(resolved, m_config.assetsDir, sceneBuilder(), m_context.deviceContext(), m_commandPool);
    if (!sceneConfig) {
        core::Logger::error("Scene load failed: {}", resolved.string());
        return false;
    }

    m_workingColorSpace = sceneConfig->workingColorSpace;
    m_tonemapper = sceneConfig->tonemapper.value_or(0U);
    applySceneStageConfig(*sceneConfig);
    core::Logger::info("Stage config: accumulation={}, denoiser={}, tonemapper={}",
                       m_config.stages.accumulation,
                       m_config.stages.denoiser,
                       m_config.stages.tonemapper);
    core::Logger::info(
        "Denoiser config: strength={:.3f}, iterations={}, history={}, history_blend={:.3f}, gradient={}, "
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
            core::Logger::warn("Unsupported post-tonemap renderer '{}' — ignoring", *sceneConfig->postTonemapRenderer);
        }
    }
    core::Logger::info("Working color space: {}", utils::ColorSpace::interopId(m_workingColorSpace));

    // IBL environment map (shared descriptor wiring lives here; renderers
    // consume the probe via iblProbe()).
    if (sceneConfig->envMapFile) {
        const auto envPath = m_config.assetsDir / *sceneConfig->envMapFile;
        auto probe =
            scene::IblProbe::loadFromEXR(m_context.deviceContext(), m_commandPool, envPath, m_workingColorSpace);
        if (!probe) {
            core::Logger::warn("IBL probe load failed for '{}' — using procedural sky", envPath.string());
        } else {
            m_iblProbe = std::move(*probe);
            if (const VkResult result = m_descriptors.updateEnvMap(
                    m_context.deviceContext(), m_iblProbe->imageView(), m_iblProbe->sampler());
                result != VK_SUCCESS) {
                core::Logger::warn("IBL descriptor update failed: VkResult {}", static_cast<int>(result));
            } else {
                core::Logger::info("IBL probe loaded: '{}'", envPath.filename().string());
            }
            if (m_iblProbe->cdfWidth() > 0) {
                if (const VkResult result = m_descriptors.updateEnvImportance(
                        m_context.deviceContext(), m_iblProbe->marginalCdfBuffer(), m_iblProbe->conditionalCdfBuffer());
                    result != VK_SUCCESS) {
                    core::Logger::warn("IBL importance descriptor update failed: VkResult {}",
                                       static_cast<int>(result));
                } else {
                    core::Logger::info("IBL importance CDF descriptors updated ({}×{})",
                                       m_iblProbe->cdfWidth(),
                                       m_iblProbe->cdfHeight());
                }
            }
        }
    }

    if (!onSceneLoaded(*sceneConfig)) {
        core::Logger::error("Renderer scene setup failed: {}", resolved.string());
        return false;
    }

    if (!createDenoisedImage() || !createToneMapper()) {
        return false;
    }
    ++m_sceneEpoch;
    rebuildStagePipeline();

    return true;
}

void App::shutdown() noexcept {
    if (m_context.deviceContext().device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_context.deviceContext().device);
        m_frameSync.destroy();
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

} // namespace harmonia::app
