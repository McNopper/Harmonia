#include "harmonia/app/App.hpp"

#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <utility>

#include "harmonia/core/Barrier.hpp"
#include "harmonia/core/Logger.hpp"
#include "harmonia/presentation/ImageCapture.hpp"
#include "harmonia/utils/ColorSpace.hpp"

namespace harmonia {

namespace {

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
        .width = static_cast<uint32_t>(std::max(width, 1)),
        .height = static_cast<uint32_t>(std::max(height, 1)),
    };
}

} // namespace

App::~App() {
    shutdown();
}

bool App::applyCommonArg(Config& config, int& i, int argc, char* const argv[]) {
    const std::string_view arg = argv[i];
    const auto next = [&](const char* what) -> const char* {
        if (i + 1 >= argc) {
            Logger::error("Missing value for {}", what);
            return nullptr;
        }
        return argv[++i];
    };

    if (arg == "--scene" || arg == "-s") {
        if (const char* v = next("--scene")) {
            config.sceneFile = v;
        }
        return true;
    }
    if (arg == "--output" || arg == "-o") {
        if (const char* v = next("--output")) {
            config.outputFile = v;
        }
        return true;
    }
    if (arg == "--width") {
        if (const char* v = next("--width")) {
            config.width = static_cast<uint32_t>(std::max(std::atoi(v), 1));
        }
        return true;
    }
    if (arg == "--height") {
        if (const char* v = next("--height")) {
            config.height = static_cast<uint32_t>(std::max(std::atoi(v), 1));
        }
        return true;
    }
    if (arg == "--validation") {
        config.validation = true;
        return true;
    }
    if (arg == "--no-validation") {
        config.validation = false;
        return true;
    }
    if (!arg.starts_with("-")) {
        config.sceneFile = std::filesystem::path(arg);
        return true;
    }
    return false;
}

int App::run(Config config) {
    m_config = std::move(config);

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

    if (!createToneMapper() || !createHdrImage()) {
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
        if (createBinarySemaphore(device, frame.imageAvailable) != VK_SUCCESS) {
            Logger::error("Semaphore creation failed");
            return false;
        }
    }
    m_renderComplete.resize(m_swapchain.imageCount());
    for (VkSemaphore& sem : m_renderComplete) {
        if (createBinarySemaphore(device, sem) != VK_SUCCESS) {
            Logger::error("renderComplete semaphore creation failed");
            return false;
        }
    }
    if (createTimelineSemaphore(device, m_timelineSemaphore) != VK_SUCCESS) {
        Logger::error("Timeline semaphore creation failed");
        return false;
    }

    return true;
}

bool App::createHdrImage() {
    // Usage covers both renderer families: storage writes (ray/compute) and
    // color-attachment + sampled reads (raster), plus read-back for capture.
    auto hdrImage = Image::create(m_context.deviceContext(),
                                  m_swapchain.extent(),
                                  VK_FORMAT_R32G32B32A32_SFLOAT,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT,
                                  "harmonia.hdr");
    if (!hdrImage) {
        Logger::error("HDR image creation failed: VkResult {}", static_cast<int>(hdrImage.error()));
        return false;
    }
    m_hdrImage = std::move(*hdrImage);
    return true;
}

bool App::createToneMapper() {
    const std::filesystem::path shaderDir = HARMONIA_SHADER_DIR;
    auto toneMapper = ToneMapper::create(
        m_context.deviceContext(), m_swapchain.format(), shaderDir / "tonemap_vert.spv", shaderDir / "tonemap.spv");
    if (!toneMapper) {
        Logger::error("Tone mapper creation failed: VkResult {}", static_cast<int>(toneMapper.error()));
        return false;
    }
    m_toneMapper = std::move(*toneMapper);
    return true;
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

    return true;
}

uint64_t App::renderSceneReferred() {
    FrameResources& frame = m_frames[m_currentFrame];

    // Wait for the previous use of this frame slot to complete.
    if (frame.completionValue > 0U) {
        const VkSemaphoreWaitInfo waitInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .pNext = nullptr,
            .flags = 0,
            .semaphoreCount = 1,
            .pSemaphores = &m_timelineSemaphore,
            .pValues = &frame.completionValue,
        };
        vkWaitSemaphores(m_context.deviceContext().device, &waitInfo, UINT64_MAX);
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

    if (vkEndCommandBuffer(frame.renderCmd) != VK_SUCCESS) {
        Logger::error("Failed to end render command buffer");
        return frame.completionValue;
    }

    const uint64_t signalValue = m_nextTimelineValue++;
    const VkCommandBufferSubmitInfo cmdInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .pNext = nullptr,
        .commandBuffer = frame.renderCmd,
        .deviceMask = 0,
    };
    const VkSemaphoreSubmitInfo timelineSignal{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .pNext = nullptr,
        .semaphore = m_timelineSemaphore,
        .value = signalValue,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .deviceIndex = 0,
    };
    const VkSubmitInfo2 submitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .pNext = nullptr,
        .flags = 0,
        .waitSemaphoreInfoCount = 0,
        .pWaitSemaphoreInfos = nullptr,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &cmdInfo,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &timelineSignal,
    };
    if (vkQueueSubmit2(m_context.deviceContext().graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS) {
        Logger::error("Render queue submit failed");
        return frame.completionValue;
    }

    frame.completionValue = signalValue;
    ++m_frameIndex;
    m_currentFrame = (m_currentFrame + 1U) % static_cast<uint32_t>(m_frames.size());
    return signalValue;
}

void App::presentFrame(uint32_t slot, uint64_t renderValue) {
    FrameResources& frame = m_frames[slot];

    uint32_t imageIndex = 0;
    VkResult result = m_swapchain.acquireNextImage(frame.imageAvailable, imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        const VkExtent2D extent = windowPixelExtent(m_window);
        handleResize(extent.width, extent.height);
        return;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        Logger::error("Swapchain acquire failed: VkResult {}", static_cast<int>(result));
        return;
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
        return;
    }

    const std::array preToneMapBarriers{
        imageBarrier(m_hdrImage.handle(),
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     renderer().outputStageMask(),
                     VK_ACCESS_2_SHADER_WRITE_BIT,
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
    pipelineBarrier(frame.displayCmd, preToneMapBarriers);

    // The shared ToneMapper is the glue between the scene-referred working
    // space and the display-referred output of the negotiated swapchain.
    m_toneMapper.record(frame.displayCmd,
                        m_hdrImage.view(),
                        m_swapchain.imageView(imageIndex),
                        m_swapchain.extent(),
                        m_swapchain.outputColorSpace(),
                        m_tonemapper,
                        m_workingColorSpace);

    const std::array presentBarrier{
        imageBarrier(m_swapchain.image(imageIndex),
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_NONE,
                     0),
    };
    pipelineBarrier(frame.displayCmd, presentBarrier);

    if (vkEndCommandBuffer(frame.displayCmd) != VK_SUCCESS) {
        Logger::error("Failed to end display command buffer");
        return;
    }

    const uint64_t displayValue = m_nextTimelineValue++;
    const std::array<VkSemaphoreSubmitInfo, 2> waitInfos{{
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = m_timelineSemaphore,
            .value = renderValue,
            .stageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .deviceIndex = 0,
        },
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = frame.imageAvailable,
            .value = 0,
            .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
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
    const VkCommandBufferSubmitInfo displayCmdInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .pNext = nullptr,
        .commandBuffer = frame.displayCmd,
        .deviceMask = 0,
    };
    const VkSubmitInfo2 displaySubmit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .pNext = nullptr,
        .flags = 0,
        .waitSemaphoreInfoCount = static_cast<uint32_t>(waitInfos.size()),
        .pWaitSemaphoreInfos = waitInfos.data(),
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &displayCmdInfo,
        .signalSemaphoreInfoCount = static_cast<uint32_t>(signalInfos.size()),
        .pSignalSemaphoreInfos = signalInfos.data(),
    };
    result = vkQueueSubmit2(m_context.deviceContext().graphicsQueue, 1, &displaySubmit, VK_NULL_HANDLE);
    if (result != VK_SUCCESS) {
        Logger::error("Display submit failed: VkResult {}", static_cast<int>(result));
        return;
    }

    // This slot must not be reused until display has also completed.
    frame.completionValue = displayValue;

    result = m_swapchain.present(m_context.deviceContext().graphicsQueue, imageIndex, m_renderComplete[imageIndex]);
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
    uint64_t lastTick = SDL_GetTicksNS();

    while (m_running) {
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            if (onEvent(event)) {
                continue;
            }
            if (event.type == SDL_EVENT_QUIT) {
                m_running = false;
            } else if (event.type == SDL_EVENT_WINDOW_RESIZED || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                const VkExtent2D extent = windowPixelExtent(m_window);
                handleResize(extent.width, extent.height);
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                m_running = false;
            }
        }

        const uint64_t now = SDL_GetTicksNS();
        const float dtSeconds = static_cast<float>(now - lastTick) * 1e-9F;
        lastTick = now;
        onUpdate(dtSeconds);

        if (!m_running || m_swapchain.extent().width == 0U || m_swapchain.extent().height == 0U) {
            continue;
        }

        // Save the frame slot before renderSceneReferred advances it.
        const uint32_t slot = m_currentFrame;
        const uint64_t renderValue = renderSceneReferred();
        presentFrame(slot, renderValue);
    }

    vkDeviceWaitIdle(m_context.deviceContext().device);
    return 0;
}

int App::renderOffscreen() {
    const uint32_t frameCount = offscreenFrameCount();
    Logger::info("Offscreen render: {} frame(s) -> {}", frameCount, m_config.outputFile.string());
    for (uint32_t i = 0; i < frameCount; ++i) {
        renderSceneReferred();
    }
    vkDeviceWaitIdle(m_context.deviceContext().device);

    bool ok = true;
    if (m_config.outputFile.extension() == ".png") {
        // Tone-mapped display-referred capture only.
        ok = ImageCapture::savePng(
            m_context.deviceContext(), m_commandPool, m_hdrImage, m_config.outputFile, m_workingColorSpace);
    } else {
        // Scene-referred linear EXR (chromaticities-tagged, untonemapped) plus
        // a tone-mapped sRGB PNG sibling for GitHub / README display.
        ok = ImageCapture::saveExr(
            m_context.deviceContext(), m_commandPool, m_hdrImage, m_config.outputFile, m_workingColorSpace);
        auto pngPath = m_config.outputFile;
        pngPath.replace_extension(".png");
        ok =
            ImageCapture::savePng(m_context.deviceContext(), m_commandPool, m_hdrImage, pngPath, m_workingColorSpace) &&
            ok;
    }
    return ok ? 0 : 1;
}

bool App::saveExr(const std::filesystem::path& path) {
    vkDeviceWaitIdle(m_context.deviceContext().device);
    return ImageCapture::saveExr(m_context.deviceContext(), m_commandPool, m_hdrImage, path, m_workingColorSpace);
}

void App::handleResize(uint32_t w, uint32_t h) {
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
    for (VkSemaphore& sem : m_renderComplete) {
        vkDestroySemaphore(device, sem, nullptr);
    }
    m_renderComplete.resize(m_swapchain.imageCount());
    for (VkSemaphore& sem : m_renderComplete) {
        if (createBinarySemaphore(device, sem) != VK_SUCCESS) {
            Logger::error("renderComplete semaphore recreate failed");
            return;
        }
    }

    if (!createHdrImage()) {
        return;
    }

    // Recreate tone mapper in case the swapchain format changed (HDR10 ↔ SDR).
    if (!createToneMapper()) {
        return;
    }

    renderer().onResize(m_swapchain.extent());
    onResized(m_swapchain.extent());
}

void App::shutdown() noexcept {
    if (m_context.deviceContext().device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_context.deviceContext().device);
        for (FrameResources& frame : m_frames) {
            if (frame.imageAvailable != VK_NULL_HANDLE) {
                vkDestroySemaphore(m_context.deviceContext().device, frame.imageAvailable, nullptr);
            }
            frame = {};
        }
        for (VkSemaphore& sem : m_renderComplete) {
            if (sem != VK_NULL_HANDLE) {
                vkDestroySemaphore(m_context.deviceContext().device, sem, nullptr);
            }
        }
        m_renderComplete.clear();
        if (m_timelineSemaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(m_context.deviceContext().device, m_timelineSemaphore, nullptr);
            m_timelineSemaphore = VK_NULL_HANDLE;
        }
    }

    m_iblProbe.reset();
    m_hdrImage = {};
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
