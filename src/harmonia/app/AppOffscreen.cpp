# AppOffscreen.cpp - the offscreen-capture path of App: scene-referred
# rendering, the offscreen frame loop, and EXR/PNG export. Methods of
# harmonia::app::App split out of App.cpp (TU split, no behavior change).
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

std::uint64_t App::renderSceneReferred() {
    const std::uint32_t slot = m_frameSync.currentSlot();

    // Wait for the previous use of this frame slot to complete.
    m_frameSync.waitSlotComplete(slot);

    const VkCommandBuffer renderCmd = m_frameSync.renderCmd(slot);
    vkResetCommandBuffer(renderCmd, 0);
    const VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    if (vkBeginCommandBuffer(renderCmd, &beginInfo) != VK_SUCCESS) {
        core::Logger::error("Failed to begin render command buffer");
        return m_frameSync.completionValue(slot);
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

    if (vkEndCommandBuffer(stagesCmd) != VK_SUCCESS) {
        core::Logger::error("Failed to end render command buffer");
        return m_frameSync.completionValue(slot);
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
    if (core::submitOneShot(m_context.deviceContext().graphicsQueue,
                      stagesCmd,
                      VK_NULL_HANDLE,
                      0,
                      waitSemaphores,
                      std::span<const VkSemaphoreSubmitInfo>{&timelineSignal, 1}) != VK_SUCCESS) {
        core::Logger::error("Render queue submit failed");
        return m_frameSync.completionValue(slot);
    }

    m_frameSync.markSlotComplete(slot, signalValue);
    ++m_frameIndex;
    m_frameSync.rotate();
    return signalValue;
}


int App::renderOffscreen() {
    const std::uint32_t frameCount = offscreenFrameCount();
    core::Logger::info("Offscreen render: {} frame(s) -> {}", frameCount, m_config.outputFile.string());

    // Headless capture is a batch job: run it BELOW_NORMAL so every other process on the
    // machine preempts it automatically and stays responsive. This is the actual
    // mechanism -- the per-frame yield below is only a hint (see the comment there).
    // Restored to the previous class on return, so an interactive session is unaffected.
    core::ScopedBackgroundPriority backgroundPriority;

    for (std::uint32_t i = 0; i < frameCount; ++i) {
        renderSceneReferred();
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
    vkDeviceWaitIdle(m_context.deviceContext().device);

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
    vkDeviceWaitIdle(m_context.deviceContext().device);
    return harmonia::presentation::ImageCapture::saveExr(
        m_context.deviceContext(), m_commandPool, sceneOutputImage(), path, m_workingColorSpace);
}



} // namespace harmonia::app
