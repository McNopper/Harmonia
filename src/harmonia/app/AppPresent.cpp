// AppPresent.cpp - the interactive display path of App: frame acquire,
// present barriers/submit, resize handling, and the main loop. Methods
// of harmonia::app::App split out of App.cpp (TU split, no behavior change).
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

#include "harmonia/app/App.hpp"
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

void App::presentFrame(std::uint32_t slot, std::uint64_t renderValue) {
    auto imageIndex = acquireFrame(slot);
    if (!imageIndex) {
        return;
    }

    recordDisplayBarriers(m_frameSync.displayCmd(slot), *imageIndex);

    const pipeline::PassContext displayPassContext{
        .cmd = m_frameSync.displayCmd(slot),
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
            core::imageBarrier(m_swapchain.image(*imageIndex),
                               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                               VK_IMAGE_LAYOUT_GENERAL,
                               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                               VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                               VK_ACCESS_2_TRANSFER_WRITE_BIT),
        };
        core::pipelineBarrier(m_frameSync.displayCmd(slot), toGeneral);
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
            m_frameSync.displayCmd(slot), m_swapchain.image(*imageIndex), VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
        swapchainInGeneral = true;
    }

    if (m_config.displayOverlay) {
        if (!m_displayOverlayLogged) {
            core::Logger::info("Display overlay renderer enabled");
            m_displayOverlayLogged = true;
        }
        const std::array displayPreBarriers{
            core::imageBarrier(m_swapchain.image(*imageIndex),
                               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                               VK_IMAGE_LAYOUT_GENERAL,
                               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                               VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                               VK_ACCESS_2_TRANSFER_WRITE_BIT),
        };
        core::pipelineBarrier(m_frameSync.displayCmd(slot), displayPreBarriers);
        swapchainInGeneral = true;

        const RenderTarget displayTarget{
            .image = m_swapchain.image(*imageIndex),
            .view = m_swapchain.imageView(*imageIndex),
            .extent = m_swapchain.extent(),
            .colorSpace = m_swapchain.outputColorSpace(),
            .workingColorSpace = m_workingColorSpace,
        };
        m_displayRenderer.record(m_frameSync.displayCmd(slot), displayTarget);
    }

    const std::array presentBarrier{
        core::imageBarrier(m_swapchain.image(*imageIndex),
                           swapchainInGeneral ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                           swapchainInGeneral ? VK_PIPELINE_STAGE_2_TRANSFER_BIT
                                              : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           swapchainInGeneral ? VK_ACCESS_2_TRANSFER_WRITE_BIT : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_NONE,
                           0),
    };
    core::pipelineBarrier(m_frameSync.displayCmd(slot), presentBarrier);

    if (submitDisplay(slot, *imageIndex, renderValue) != VK_SUCCESS) {
        return;
    }

    presentAndHandleResize(*imageIndex);
}

std::optional<std::uint32_t> App::acquireFrame(std::uint32_t slot) noexcept {

    std::uint32_t imageIndex = 0;
    VkResult result = m_swapchain.acquireNextImage(m_frameSync.imageAvailable(slot), imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        const VkExtent2D extent = windowPixelExtent(m_window);
        handleResize(extent.width, extent.height);
        return std::nullopt;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        core::Logger::error("presentation::Swapchain acquire failed: VkResult {}", static_cast<int>(result));
        return std::nullopt;
    }

    vkResetCommandBuffer(m_frameSync.displayCmd(slot), 0);
    const VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    if (vkBeginCommandBuffer(m_frameSync.displayCmd(slot), &beginInfo) != VK_SUCCESS) {
        core::Logger::error("Failed to begin display command buffer");
        return std::nullopt;
    }
    return imageIndex;
}

void App::recordDisplayBarriers(VkCommandBuffer cmd, std::uint32_t imageIndex) noexcept {
    const std::array preToneMapBarriers{
        core::imageBarrier(sceneOutputImage().handle(),
                           VK_IMAGE_LAYOUT_GENERAL,
                           VK_IMAGE_LAYOUT_GENERAL,
                           sceneOutputStageMask(),
                           sceneOutputAccessMask(),
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_2_SHADER_READ_BIT),
        core::imageBarrier(m_swapchain.image(imageIndex),
                           m_frameSync.swapchainLayout(imageIndex),
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_PIPELINE_STAGE_2_NONE,
                           0,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT),
    };
    core::pipelineBarrier(cmd, preToneMapBarriers);
}

VkResult App::submitDisplay(std::uint32_t slot, std::uint32_t imageIndex, std::uint64_t renderValue) noexcept {

    if (vkEndCommandBuffer(m_frameSync.displayCmd(slot)) != VK_SUCCESS) {
        core::Logger::error("Failed to end display command buffer");
        return VK_ERROR_UNKNOWN;
    }

    const std::uint64_t displayValue = m_frameSync.nextSignalValue();
    const VkPipelineStageFlags2 sceneReadyWaitStage =
        hasTonemapStage() ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    const VkPipelineStageFlags2 imageAcquireWaitStage =
        hasTonemapStage() ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    const std::array<VkSemaphoreSubmitInfo, 2> waitInfos{{
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = m_frameSync.timeline(),
            .value = renderValue,
            .stageMask = sceneReadyWaitStage,
            .deviceIndex = 0,
        },
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = m_frameSync.imageAvailable(slot),
            .value = 0,
            .stageMask = imageAcquireWaitStage,
            .deviceIndex = 0,
        },
    }};
    const std::array<VkSemaphoreSubmitInfo, 2> signalInfos{{
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = m_frameSync.renderComplete(imageIndex),
            .value = 0,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .deviceIndex = 0,
        },
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = m_frameSync.timeline(),
            .value = displayValue,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .deviceIndex = 0,
        },
    }};
    const VkResult result = core::submitOneShot(m_context.deviceContext().graphicsQueue,
                                                m_frameSync.displayCmd(slot),
                                                VK_NULL_HANDLE,
                                                0,
                                                waitInfos,
                                                signalInfos);
    if (result != VK_SUCCESS) {
        core::Logger::error("Display submit failed: VkResult {}", static_cast<int>(result));
        return result;
    }

    // This slot must not be reused until display has also completed.
    m_frameSync.markSlotComplete(slot, displayValue);
    return VK_SUCCESS;
}

void App::presentAndHandleResize(std::uint32_t imageIndex) noexcept {
    const VkResult result = m_swapchain.present(
        m_context.deviceContext().graphicsQueue, imageIndex, m_frameSync.renderComplete(imageIndex));
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        m_frameSync.setSwapchainLayout(imageIndex, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        const VkExtent2D extent = windowPixelExtent(m_window);
        handleResize(extent.width, extent.height);
        return;
    }
    if (result != VK_SUCCESS) {
        core::Logger::error("Present failed: VkResult {}", static_cast<int>(result));
    }
    m_frameSync.setSwapchainLayout(imageIndex, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

int App::mainLoop() {
    core::Logger::info("Interactive render loop");
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
        const std::uint32_t slot = m_frameSync.currentSlot();
        const std::uint64_t renderValue = renderSceneReferred();
        // I6: --frames-per-flip — N accumulation frames per swapchain flip. The render
        // half runs every iteration; only the display/present half is gated, so N = 1 is
        // exactly the classic render-then-present cadence.
        ++m_framesSinceFlip;
        if (m_framesSinceFlip >= std::max(m_config.framesPerFlip, 1U)) {
            m_framesSinceFlip = 0U;
            presentFrame(slot, renderValue);
        }
        // Courtesy yield, same rationale as renderOffscreen(). `presentFrame` paces this
        // loop under FIFO present, but with MAILBOX/immediate it does not block and the
        // window then pins the CPU exactly like an offscreen capture. Costs ~nothing.
        std::this_thread::yield();
    }

    vkDeviceWaitIdle(m_context.deviceContext().device);
    return 0;
}

void App::handleResize(std::uint32_t w, std::uint32_t h) {
    if (w == 0U || h == 0U || m_context.deviceContext().device == VK_NULL_HANDLE) {
        return;
    }

    // MOD4: driver-side scaling was attempted (VkSwapchainPresentScalingCreateInfoKHR),
    // but the RTX 5070 driver 616.92 reports supportedPresentScaling = 0 for all present
    // modes — scaling is not available. The recreate path below is the proven behavior.

    vkDeviceWaitIdle(m_context.deviceContext().device);

    // Reset frame command buffers (to INITIAL) + completion values before destroying any
    // resources they reference. (Timeline signal values stay monotonic across a resize.)
    m_frameSync.resetSlots();

    const VkExtent2D newExtent{
        .width = w,
        .height = h,
    };
    if (m_swapchain.recreate(newExtent) != VK_SUCCESS) {
        core::Logger::error("presentation::Swapchain recreate failed");
        return;
    }
    // Recreate per-image render-complete semaphores + reset layout tracking for the new
    // swapchain image count. vkDeviceWaitIdle() above guarantees they are unsignaled.
    m_frameSync.onResize(m_swapchain.imageCount());

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

} // namespace harmonia::app
