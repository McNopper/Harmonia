// AppOffscreen.cpp - the offscreen-capture path of App: scene-referred
// rendering, the offscreen frame loop, and EXR/PNG export. Methods of
// harmonia::app::App split out of App.cpp (TU split, no behavior change).
#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
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

int App::renderOffscreen() {
    const std::uint32_t frameCount = offscreenFrameCount();
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
    for (std::uint32_t i = 0; i < frameCount; ++i) {
        const auto frameStarted = std::chrono::steady_clock::now();
        const auto rendered = renderSceneReferred();
        if (!rendered) {
            core::Logger::error("Offscreen frame failed; no incomplete capture will be exported");
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
    if (vkDeviceWaitIdle(m_context.deviceContext().device) != VK_SUCCESS) {
        static_cast<void>(core::waitDeviceQuiescent(deviceContext().device));
        return 1;
    }

    const bool isPng = (m_config.outputFile.extension() == ".png");
    bool ok = true;

    // Scene-referred linear EXR (chromaticities-tagged, untonemapped) when not a bare .png.
    if (!isPng) {
        ok = harmonia::presentation::ImageCapture::saveExr(
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
    if (m_toneMapper.isValid() && m_offscreen.tonemapToCaptureImage(deviceContext(),
                                                                    commandPool(),
                                                                    m_swapchain.extent(),
                                                                    sceneOutputImage(),
                                                                    sceneOutputStageMask(),
                                                                    sceneOutputAccessMask(),
                                                                    m_tonemapper,
                                                                    m_workingColorSpace)) {
        // Capture image is VK_FORMAT_R8G8B8A8_UNORM (RGBA) — no channel swap needed.
        ok = harmonia::presentation::ImageCapture::saveSdrPng(
                 m_context.deviceContext(), m_commandPool, m_offscreen.captureImage(), pngPath, /*swapRB=*/false) &&
             ok;
    } else {
        ok = harmonia::presentation::ImageCapture::savePng(
                 m_context.deviceContext(), m_commandPool, sceneOutputImage(), pngPath, m_workingColorSpace) &&
             ok;
    }
    return ok ? 0 : 1;
}

bool App::saveExr(const std::filesystem::path& path) {
    if (core::waitDeviceQuiescent(m_context.deviceContext().device) != VK_SUCCESS)
        return false;
    return harmonia::presentation::ImageCapture::saveExr(
        m_context.deviceContext(), m_commandPool, sceneOutputImage(), path, m_workingColorSpace);
}

} // namespace harmonia::app
