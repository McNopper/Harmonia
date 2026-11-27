// AppOffscreen.cpp - the offscreen-capture path of App: scene-referred
// rendering, the offscreen frame loop, and EXR/PNG export. Methods of
// harmonia::app::App split out of App.cpp (TU split, no behavior change).
#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "harmonia/app/App.hpp"
#include "harmonia/core/Barrier.hpp"
#include "harmonia/core/Env.hpp"
#include "harmonia/core/Logger.hpp"
#include "harmonia/core/OneShot.hpp"
#include "harmonia/core/ProcessPriority.hpp"
#include "harmonia/pipeline/AccumulationPass.hpp"
#include "harmonia/pipeline/PassContext.hpp"
#include "harmonia/pipeline/SceneOutputCopyPass.hpp"
#include "harmonia/presentation/ImageCapture.hpp"
#include "harmonia/utils/ColorSpace.hpp"
#include "harmonia/vulkan_init/DeviceFault.hpp"

namespace harmonia::app {

namespace {

/// ANI7: per-frame output naming for sequence renders - `<stem>_0000<ext>`,
/// `<stem>_0001<ext>`, ... beside the classic single-capture path.
[[nodiscard]] std::filesystem::path sequenceOutputPath(const std::filesystem::path& base, std::uint32_t frame) {
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), "_%04u", frame);
    std::filesystem::path out = base;
    out.replace_filename(base.stem().string() + suffix + base.extension().string());
    return out;
}

} // namespace

std::expected<std::uint64_t, VkResult> App::renderSceneReferred() {
    const std::uint32_t slot = m_frameSync.currentSlot();

    // Wait for the previous use of this frame slot to complete.
    if (const auto result = m_frameSync.waitSlotComplete(slot); result != VK_SUCCESS)
        return std::unexpected(result);

    const VkCommandBuffer renderCmd = m_frameSync.renderCmd(slot);
    vkResetCommandBuffer(renderCmd, 0);
    const VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    if (const auto result = vkBeginCommandBuffer(renderCmd, &beginInfo); result != VK_SUCCESS) {
        core::Logger::error("Failed to begin render command buffer");
        return std::unexpected(result);
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
    renderer().record(renderCmd, target);

    // Allow subclasses to split the frame for async compute.
    // Default: {renderCmd, VK_NULL_HANDLE} — single-queue path unchanged.
    auto [stagesCmd, asyncWaitSem] = onBeforeSceneStages(renderCmd);

    if (auto* completion = sceneFrameCompletion()) {
        // Finish the initial producer batch, including async ownership-acquire
        // barriers, BEFORE querying its GPU-written completion status. This path
        // is absent for ordinary one-submission renderers.
        if (const auto result = vkEndCommandBuffer(stagesCmd); result != VK_SUCCESS)
            return std::unexpected(result);
        VkSemaphore rawTimeline{};
        if (const auto result = core::createTimelineSemaphore(deviceContext().device, &rawTimeline);
            result != VK_SUCCESS)
            return std::unexpected(result);
        const core::UniqueSemaphore timeline{deviceContext().device, rawTimeline};
        const VkSemaphoreSubmitInfo waitInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                             .pNext = nullptr,
                                             .semaphore = asyncWaitSem,
                                             .value = 0,
                                             .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                             .deviceIndex = 0};
        const auto waits = asyncWaitSem == VK_NULL_HANDLE ? std::span<const VkSemaphoreSubmitInfo>{}
                                                          : std::span<const VkSemaphoreSubmitInfo>{&waitInfo, 1};
        if (const auto result = core::submitOneShot(deviceContext().graphicsQueue, stagesCmd, timeline, 1, waits);
            result != VK_SUCCESS) {
            static_cast<void>(core::waitDeviceQuiescent(deviceContext().device));
            return std::unexpected(result);
        }
        if (const auto result = core::waitTimelineSemaphore(deviceContext().device, timeline, 1);
            result != VK_SUCCESS) {
            // Retire BEFORE the local timeline owner unwinds, not only in the
            // outer frame loop. A failed wait may leave this submission in flight.
            static_cast<void>(core::waitDeviceQuiescent(deviceContext().device));
            return std::unexpected(result);
        }
        if (const auto result = drainFrameWork(*completion, m_commandPool); result != VK_SUCCESS) {
            core::Logger::error("Renderer frame completion failed: {}", static_cast<int>(result));
            return std::unexpected(result);
        }
        if (const auto result = vkResetCommandBuffer(stagesCmd, 0); result != VK_SUCCESS)
            return std::unexpected(result);
        if (const auto result = vkBeginCommandBuffer(stagesCmd, &beginInfo); result != VK_SUCCESS)
            return std::unexpected(result);
        asyncWaitSem = VK_NULL_HANDLE; // The binary semaphore was consumed above.
    }
    onSceneFrameReady(stagesCmd);

    const pipeline::PassContext passContext{
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

    if (const auto result = vkEndCommandBuffer(stagesCmd); result != VK_SUCCESS) {
        core::Logger::error("Failed to end render command buffer");
        return std::unexpected(result);
    }

    const std::uint64_t signalValue = m_frameSync.nextSignalValue();
    const VkSemaphoreSubmitInfo timelineSignal{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .pNext = nullptr,
        .semaphore = m_frameSync.timeline(),
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
    const VkResult submitResult = core::submitOneShot(m_context.deviceContext().graphicsQueue,
                                                      stagesCmd,
                                                      VK_NULL_HANDLE,
                                                      0,
                                                      waitSemaphores,
                                                      std::span<const VkSemaphoreSubmitInfo>{&timelineSignal, 1});
    if (submitResult != VK_SUCCESS) {
        core::Logger::error("Render queue submit failed");
        // VK16: device-lost forensics — diagnosability only, never masks the error.
        if (submitResult == VK_ERROR_DEVICE_LOST) {
            vulkan_init::reportDeviceFault(m_context.deviceContext());
        }
        return std::unexpected(submitResult);
    }

    m_frameSync.markSlotComplete(slot, signalValue);
    ++m_frameIndex;
    m_frameSync.rotate();
    return signalValue;
}

namespace {

// SplitMix64 finalizer over the seed, the output frame and the stratum: a
// uniform in [0, 1) that is reproducible from --rng-seed.
[[nodiscard]] double stratumJitter(std::uint32_t seed, std::uint32_t frame, std::uint32_t stratum) noexcept {
    std::uint64_t z = (static_cast<std::uint64_t>(seed) * 0x9E3779B97F4A7C15ULL) ^
                      (static_cast<std::uint64_t>(frame) + 1ULL) * 0xBF58476D1CE4E5B9ULL ^
                      (static_cast<std::uint64_t>(stratum) + 1ULL) * 0x94D049BB133111EBULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    return static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
}

} // namespace

App::AnimationClock App::sequenceSampleClock(const AppConfig& config,
                                             double fps,
                                             std::uint32_t outputFrame,
                                             std::uint32_t sample) noexcept {
    const double safeFps = fps > 0.0 ? fps : 30.0;
    const std::uint32_t tick = config.animationStartFrame + outputFrame;
    AnimationClock clock;
    double frameInterval;
    if (config.captureIntervalMs > 0.0) {
        frameInterval = config.captureIntervalMs / 1000.0;
        const double start = config.captureStartSeconds >= 0.0
                                 ? config.captureStartSeconds
                                 : static_cast<double>(config.animationStartFrame) / safeFps;
        clock.seconds = start + static_cast<double>(outputFrame) * frameInterval;
        clock.key = static_cast<std::uint64_t>(std::llround(clock.seconds * 1'000'000.0));
    } else {
        frameInterval = 1.0 / safeFps;
        clock.seconds = static_cast<double>(tick) / safeFps;
        clock.key = tick;
    }

    if (config.shutter > 0.0f && config.timeSamples > 1U) {
        const std::uint32_t stratum = sample % config.timeSamples;
        const double open = static_cast<double>(config.shutter) * frameInterval;
        clock.seconds += open * (static_cast<double>(stratum) + stratumJitter(config.rngSeed, outputFrame, stratum)) /
                         static_cast<double>(config.timeSamples);
        clock.key = static_cast<std::uint64_t>(std::llround(clock.seconds * 1'000'000.0));
    }
    return clock;
}

int App::renderOffscreen() {
    if (m_config.captureIntervalMs > 0.0 && m_config.sequenceFrames == 0U) {
        // Time-based capture only makes sense as a SEQUENCE of samples (the
        // classic single capture accumulates one state; the interval would
        // advance it per accumulation sample - the warned anti-pattern).
        core::Logger::error(
            "--capture-every-ms requires --sequence-frames (a time-based capture is a sequence of samples)");
        return 1;
    }
    const std::uint32_t frameCount = offscreenFrameCount();

    // A shutter integrates motion over the exposure: it needs a sequence (a
    // still has no motion to integrate), more than one time sample (one sample
    // is an instant) and an accumulation sample for each time sample.
    if (m_config.shutter > 0.0f) {
        if (m_config.sequenceFrames == 0U) {
            core::Logger::error("--shutter requires --sequence-frames: a still has no motion to integrate");
            return 1;
        }
        if (m_config.timeSamples < 2U) {
            core::Logger::error("--shutter requires --time-samples >= 2: one time sample is an instant");
            return 1;
        }
        if (frameCount < m_config.timeSamples) {
            core::Logger::error("--time-samples ({}) exceeds the accumulation samples per output frame ({}) - each "
                                "time sample needs at least one; raise --offscreen-frames or lower --time-samples",
                                m_config.timeSamples,
                                frameCount);
            return 1;
        }
    } else if (m_config.timeSamples > 1U) {
        core::Logger::error(
            "--time-samples requires --shutter > 0: without a shutter every time sample renders the same instant");
        return 1;
    }
    core::Logger::info("Offscreen render: {} frame(s) -> {}", frameCount, m_config.outputFile.string());

    // VK21 guard: warn when a device-local heap is near its budget before a large
    // capture — the display-GPU DWM demotion scenario (driver pages render targets
    // out and interactive use of the machine degrades while the capture runs).
    {
        const auto& ctx = m_context.deviceContext();
        if (ctx.memoryBudgetSupported) {
            VkPhysicalDeviceMemoryBudgetPropertiesEXT budgetProps{};
            budgetProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
            VkPhysicalDeviceMemoryProperties2 memProps2{};
            memProps2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
            memProps2.pNext = &budgetProps;
            vkGetPhysicalDeviceMemoryProperties2(ctx.physicalDevice, &memProps2);
            for (std::uint32_t heap = 0; heap < memProps2.memoryProperties.memoryHeapCount; ++heap) {
                const VkMemoryHeap& heapProps = memProps2.memoryProperties.memoryHeaps[heap];
                if ((heapProps.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0U || budgetProps.heapBudget[heap] == 0U) {
                    continue;
                }
                const auto usage = static_cast<double>(budgetProps.heapUsage[heap]);
                const auto budget = static_cast<double>(budgetProps.heapBudget[heap]);
                if (usage / budget > 0.9) {
                    core::Logger::warn("GPU memory heap {} at {:.0f}% of budget ({} / {} MiB) before capture — "
                                       "expect paging; interactive use may degrade",
                                       heap,
                                       usage * 100.0 / budget,
                                       usage / (1024.0 * 1024.0),
                                       budget / (1024.0 * 1024.0));
                }
            }
        }
    }

    // Headless capture is a batch job: run it BELOW_NORMAL so every other process on the
    // machine preempts it automatically and stays responsive. This is the actual
    // mechanism -- the per-frame yield below is only a hint (see the comment there).
    // Restored to the previous class on return, so an interactive session is unaffected.
    core::ScopedBackgroundPriority backgroundPriority;

    const bool frameTiming = core::getEnvString("HARMONIA_FRAME_TIMING") == std::optional<std::string>{"1"};

    // One OUTPUT frame: render `count` accumulation frames. Returns 0 = ok, 1 = failed
    // (already logged + device quiesced).
    const auto renderBatch = [&](std::uint32_t count) -> int {
        for (std::uint32_t i = 0; i < count; ++i) {
            m_sequenceSample = i; // with a shutter, this places the sample inside the exposure
            const auto frameStarted = std::chrono::steady_clock::now();
            const auto rendered = renderSceneReferred();
            if (!rendered) {
                core::Logger::error("Offscreen frame failed; aborting the capture (earlier sequence frames, if "
                                    "any, remain saved)");
                // Derived renderer resources are destroyed before App's base
                // destructor: retire earlier slots before returning to their owner.
                static_cast<void>(core::waitDeviceQuiescent(deviceContext().device));
                return 1;
            }
            if (frameTiming) {
                // Qualification only: observe this logical frame's complete steady work,
                // excluding process startup and image encoding. No timing-dependent
                // sampling, ownership or dispatch decision is made here.
                if (const auto result =
                        core::waitTimelineSemaphore(deviceContext().device, m_frameSync.timeline(), *rendered);
                    result != VK_SUCCESS) {
                    static_cast<void>(core::waitDeviceQuiescent(deviceContext().device));
                    return 1;
                }
                const auto elapsed =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStarted).count();
                core::Logger::info("frame_timing_ms:{}:{:.6f}", i, elapsed);
            }
            // Courtesy yield between frames, in addition to the priority drop above. It is
            // NOT sufficient on its own: on Windows this is SwitchToThread(), which per
            // Microsoft "will not switch execution to another processor, even if that
            // processor is idle or is running a thread of lower priority", and returns
            // immediately when no thread is ready -- so a yield-only loop still burns 100%
            // CPU. Kept because it is free and helps the CPU-bound stretches between frame
            // slot waits; the priority class is what actually keeps the machine usable.
            //
            // NB: neither of these is licence to run two captures at once. One GPU job at a
            // time -- see AGENTS.md.
            std::this_thread::yield();
        }
        return 0;
    };

    // ANI7 sequence render: one INDEPENDENT accumulation per animation frame. The
    // animation tick is pinned to the OUTPUT frame (animationFrameIndex()), not the
    // accumulation sample index, so all `frameCount` samples of output frame k render
    // the same scene state at t = k / animationFps - unless a shutter is set, in
    // which case the samples are spread over the exposure at their own times
    // (sequenceSampleClock) and still accumulate into the one frame. Each output
    // frame restarts accumulation via resetAccumulation() - static scenes included,
    // the frames are independent captures.
    if (m_config.sequenceFrames > 0U) {
        core::Logger::info("ANI7 sequence: {} animation frame(s) x {} accumulation frame(s) -> '{}_0000{}'",
                           m_config.sequenceFrames,
                           frameCount,
                           m_config.outputFile.stem().string(),
                           m_config.outputFile.extension().string());
        if (m_config.shutter > 0.0f) {
            core::Logger::info("Shutter: {:.2f} of the frame interval, {} time sample(s) per output frame",
                               m_config.shutter,
                               m_config.timeSamples);
        }
        m_sequenceActive = true;
        bool ok = true;
        for (std::uint32_t k = 0U; k < m_config.sequenceFrames; ++k) {
            m_sequenceFrame = k;
            resetAccumulation();
            if (const int failed = renderBatch(frameCount); failed != 0) {
                return failed;
            }
            if (vkDeviceWaitIdle(m_context.deviceContext().device) != VK_SUCCESS) {
                static_cast<void>(core::waitDeviceQuiescent(deviceContext().device));
                return 1;
            }
            ok = saveOffscreenOutputs(sequenceOutputPath(m_config.outputFile, k)) && ok;
        }
        m_sequenceActive = false;
        return ok ? 0 : 1;
    }

    if (const int failed = renderBatch(frameCount); failed != 0) {
        return failed;
    }
    if (vkDeviceWaitIdle(m_context.deviceContext().device) != VK_SUCCESS) {
        static_cast<void>(core::waitDeviceQuiescent(deviceContext().device));
        return 1;
    }
    return saveOffscreenOutputs(m_config.outputFile) ? 0 : 1;
}

bool App::saveOffscreenOutputs(const std::filesystem::path& outputFile) {
    const bool isPng = (outputFile.extension() == ".png");
    bool ok = true;

    // Scene-referred linear EXR (chromaticities-tagged, untonemapped) when not a bare .png.
    if (!isPng) {
        ok = harmonia::presentation::ImageCapture::saveExr(
            m_context.deviceContext(), m_commandPool, sceneOutputImage(), outputFile, m_workingColorSpace);
    }

    // Tone-mapped PNG. To match the interactive window, run the SAME tone mapper operator
    // (m_tonemapper) into a fixed 8-bit sRGB capture image and read that back - instead of a
    // separate, hardcoded CPU tone map. Falls back to the CPU ACES path only when the tone
    // mapper stage is unavailable.
    std::filesystem::path pngPath = outputFile;
    if (!isPng) {
        pngPath.replace_extension(".png");
    }
    if (m_toneMapper.isValid() && m_offscreen.tonemapToCaptureImage(deviceContext(),
                                                                    commandPool(),
                                                                    m_swapchain.extent(),
                                                                    sceneOutputImage(),
                                                                    sceneOutputStageMask(),
                                                                    sceneOutputAccessMask(),
                                                                    m_tonemapper,
                                                                    m_workingColorSpace,
                                                                    m_glarePass,
                                                                    m_config.glare)) {
        // Capture image is VK_FORMAT_R8G8B8A8_UNORM (RGBA) - no channel swap needed.
        ok = harmonia::presentation::ImageCapture::saveSdrPng(
                 m_context.deviceContext(), m_commandPool, m_offscreen.captureImage(), pngPath, /*swapRB=*/false) &&
             ok;
    } else {
        ok = harmonia::presentation::ImageCapture::savePng(
                 m_context.deviceContext(), m_commandPool, sceneOutputImage(), pngPath, m_workingColorSpace) &&
             ok;
    }
    return ok;
}

bool App::saveExr(const std::filesystem::path& path) {
    if (core::waitDeviceQuiescent(m_context.deviceContext().device) != VK_SUCCESS)
        return false;
    return harmonia::presentation::ImageCapture::saveExr(
        m_context.deviceContext(), m_commandPool, sceneOutputImage(), path, m_workingColorSpace);
}

} // namespace harmonia::app
