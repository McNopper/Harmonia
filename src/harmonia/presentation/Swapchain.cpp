#include "harmonia/presentation/Swapchain.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "harmonia/core/Logger.hpp"

namespace harmonia::presentation {

namespace {
constexpr std::uint64_t kWaitForever = UINT64_MAX;
/// MOD4 v1: bounded wait for pending present fences in @ref recreate. Long enough to
/// absorb normal FIFO latency; short enough that a stalled presentation engine falls
/// into the logged device-idle safety net instead of hanging the resize.
constexpr std::uint64_t kPresentFenceTimeoutNs = 250000000;

[[nodiscard]] VkExtent2D clampExtent(const VkSurfaceCapabilitiesKHR& capabilities, VkExtent2D requested) noexcept {
    if (capabilities.currentExtent.width != UINT32_MAX) {
        return capabilities.currentExtent;
    }
    return VkExtent2D{
        .width = std::clamp(requested.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
        .height = std::clamp(requested.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height),
    };
}

[[nodiscard]] VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& modes,
                                                 bool fifoLatestReadySupported) noexcept {
    // FIFO_LATEST_READY: FIFO v-sync, latest-ready image — lower latency (VK6, v0.7.6).
    // MOD4 scaling was attempted but the driver reports supportedPresentScaling = 0 for all
    // modes; scaling is not available. Falls back to spec-guaranteed FIFO.
    if (fifoLatestReadySupported &&
        std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_FIFO_LATEST_READY_KHR) != modes.end()) {
        return VK_PRESENT_MODE_FIFO_LATEST_READY_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

[[nodiscard]] std::uint32_t chooseImageCount(const VkSurfaceCapabilitiesKHR& capabilities) noexcept {
    std::uint32_t desired = std::max(2U, capabilities.minImageCount + 1U);
    if (capabilities.maxImageCount > 0U) {
        desired = std::min(desired, capabilities.maxImageCount);
    }
    return desired;
}
} // namespace

std::expected<Swapchain, VkResult> Swapchain::create(const DeviceContext& ctx,
                                                     VkSurfaceKHR surface,
                                                     VkExtent2D extent,
                                                     bool preferHDR,
                                                     VkSwapchainKHR oldSwapchain) {
    if (!ctx.isValid() || surface == VK_NULL_HANDLE || extent.width == 0U || extent.height == 0U) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    Swapchain swapchain;
    swapchain.m_ctx = &ctx;
    swapchain.m_surface = surface;
    swapchain.m_physicalDevice = ctx.physicalDevice;
    swapchain.m_preferHDR = preferHDR;

    VkSurfaceCapabilitiesKHR capabilities{};
    VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.physicalDevice, surface, &capabilities);
    if (result != VK_SUCCESS) {
        return std::unexpected(result);
    }

    std::uint32_t formatCount = 0;
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physicalDevice, surface, &formatCount, nullptr);
    if (result != VK_SUCCESS || formatCount == 0U) {
        return std::unexpected(result == VK_SUCCESS ? VK_ERROR_FORMAT_NOT_SUPPORTED : result);
    }
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physicalDevice, surface, &formatCount, formats.data());
    if (result != VK_SUCCESS) {
        return std::unexpected(result);
    }

    std::uint32_t presentModeCount = 0;
    result = vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.physicalDevice, surface, &presentModeCount, nullptr);
    if (result != VK_SUCCESS || presentModeCount == 0U) {
        return std::unexpected(result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : result);
    }
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    result =
        vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.physicalDevice, surface, &presentModeCount, presentModes.data());
    if (result != VK_SUCCESS) {
        return std::unexpected(result);
    }

    const auto findSurfaceFormat = [&](VkFormat format, VkColorSpaceKHR colorSpace) -> const VkSurfaceFormatKHR* {
        for (const VkSurfaceFormatKHR& candidate : formats) {
            if (candidate.format == format && candidate.colorSpace == colorSpace) {
                return &candidate;
            }
        }
        return nullptr;
    };

    const VkSurfaceFormatKHR* chosenFormat = nullptr;
    if (preferHDR) {
        // Priority: HDR10 PQ → HLG → scRGB → Display P3 → SDR linear
        chosenFormat = findSurfaceFormat(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT);
        if (!chosenFormat)
            chosenFormat = findSurfaceFormat(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_HLG_EXT);
        if (!chosenFormat)
            chosenFormat = findSurfaceFormat(VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT);
        if (!chosenFormat)
            chosenFormat = findSurfaceFormat(VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT);
        if (!chosenFormat)
            chosenFormat = findSurfaceFormat(VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_BT709_LINEAR_EXT);
    }
    // SDR fallback — always available
    if (!chosenFormat)
        chosenFormat = findSurfaceFormat(VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
    if (!chosenFormat)
        chosenFormat = &formats.front();

    // H-05 seam guard: the last-resort `formats.front()` fallback above can hand
    // us a color space the ToneMapper has no encoding for (e.g. Display-P3
    // LINEAR). Presenting it through the sRGB tonemap path would silently apply
    // the wrong transfer function — refuse the swapchain instead.
    if (!mapOutputColorSpace(chosenFormat->colorSpace)) {
        core::Logger::error("Swapchain: surface color space {} has no OutputColorSpace tonemap encoding — refusing a "
                            "swapchain that would present with the wrong transfer function",
                            static_cast<int>(chosenFormat->colorSpace));
        return std::unexpected(VK_ERROR_FORMAT_NOT_SUPPORTED);
    }

    swapchain.m_format = chosenFormat->format;
    swapchain.m_colorSpace = chosenFormat->colorSpace;
    swapchain.m_extent = clampExtent(capabilities, extent);

    // MOD4 v1: the fence/release half of VK_KHR_swapchain_maintenance1 is enabled (see
    // Swapchain::present / recreate) — per-present fences + image release let the resize
    // path retire the old swapchain without a device idle. The driver-side present
    // scaling half stays out: VkSwapchainPresentScalingCreateInfoKHR was probed and the
    // RTX 5070 driver 621.01 reports supportedPresentScaling = 0 for every present mode.
    // Resize therefore keeps the recreate-on-resize behavior (the proven path), just
    // without the whole-device wait.

    // COLOR_ATTACHMENT_BIT is always supported on swapchain images (Vulkan spec §34.2.2).
    // TRANSFER_DST_BIT is always supported when the surface supports presentation.
    const VkImageUsageFlags imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    // MOD6: chaining VkPresentId2KHR at present time requires the swapchain to opt in
    // (VUID-VkPresentId2KHR-None-10820) — same condition as the per-present ID tagging.
    VkSwapchainCreateFlagsKHR swapchainFlags = 0;
    if (ctx.presentIdSupported) {
        swapchainFlags |= VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR;
    }

    const VkSwapchainCreateInfoKHR createInfo{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext = nullptr,
        .flags = swapchainFlags,
        .surface = surface,
        .minImageCount = chooseImageCount(capabilities),
        .imageFormat = swapchain.m_format,
        .imageColorSpace = swapchain.m_colorSpace,
        .imageExtent = swapchain.m_extent,
        .imageArrayLayers = 1,
        .imageUsage = imageUsage,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = choosePresentMode(presentModes, ctx.fifoLatestReadySupported),
        .clipped = VK_TRUE,
        .oldSwapchain = oldSwapchain,
    };
    VkSwapchainKHR swapchainHandle{};
    result = vkCreateSwapchainKHR(ctx.device, &createInfo, nullptr, &swapchainHandle);
    if (result != VK_SUCCESS) {
        return std::unexpected(result);
    }
    swapchain.m_swapchain = harmonia::core::UniqueSwapchainKHR{ctx.device, swapchainHandle};

    std::uint32_t imageCount = 0;
    result = vkGetSwapchainImagesKHR(ctx.device, swapchain.m_swapchain, &imageCount, nullptr);
    if (result != VK_SUCCESS || imageCount == 0U) {
        return std::unexpected(result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : result);
    }
    swapchain.m_images.resize(imageCount);
    result = vkGetSwapchainImagesKHR(ctx.device, swapchain.m_swapchain, &imageCount, swapchain.m_images.data());
    if (result != VK_SUCCESS) {
        return std::unexpected(result);
    }

    swapchain.m_views.reserve(imageCount);
    for (std::uint32_t i = 0; i < imageCount; ++i) {
        const VkImageViewCreateInfo viewInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = swapchain.m_images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = swapchain.m_format,
            .components =
                VkComponentMapping{
                    .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                    .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                    .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                    .a = VK_COMPONENT_SWIZZLE_IDENTITY,
                },
            .subresourceRange =
                VkImageSubresourceRange{
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
        };
        VkImageView view = VK_NULL_HANDLE;
        result = vkCreateImageView(ctx.device, &viewInfo, nullptr, &view);
        if (result != VK_SUCCESS) {
            return std::unexpected(result);
        }
        swapchain.m_views.emplace_back(ctx.device, view);
    }

    // MOD4 v1 (VK_KHR_swapchain_maintenance1 fence/release half): one present fence
    // per swapchain image slot, recreated with the swapchain. The fence signals when
    // the presentation engine is done with that slot's image — what lets @ref recreate
    // retire the old swapchain without a whole-device idle. The per-slot bookkeeping
    // vectors are sized with the fences so slot indexing is uniform everywhere.
    if (ctx.swapchainMaintenance1Supported) {
        swapchain.m_presentFences.reserve(imageCount);
        for (std::uint32_t i = 0; i < imageCount; ++i) {
            const VkFenceCreateInfo fenceInfo{
                .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
            };
            VkFence fence = VK_NULL_HANDLE;
            result = vkCreateFence(ctx.device, &fenceInfo, nullptr, &fence);
            if (result != VK_SUCCESS) {
                return std::unexpected(result);
            }
            swapchain.m_presentFences.emplace_back(ctx.device, fence);
        }
        swapchain.m_presentFencePending.assign(imageCount, false);
        swapchain.m_acquired.assign(imageCount, false);
    }

    return swapchain;
}

VkResult Swapchain::acquireNextImage(VkSemaphore signalSemaphore, std::uint32_t& outIndex) {
    if (m_ctx == nullptr || m_swapchain == VK_NULL_HANDLE) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkResult result =
        vkAcquireNextImageKHR(m_ctx->device, m_swapchain, kWaitForever, signalSemaphore, VK_NULL_HANDLE, &outIndex);
    // MOD4 v1: track the acquired-not-presented slots — recreate() releases exactly
    // these when it retires the swapchain (maintenance1 fence/release half).
    if ((result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) && m_ctx->swapchainMaintenance1Supported) {
        m_acquired[outIndex] = true;
    }
    return result;
}

VkResult Swapchain::present(VkQueue queue, std::uint32_t imageIndex, VkSemaphore waitSemaphore) {
    // MOD6 / VK_KHR_present_id2: tag this present with a monotonic ID so
    // VK_KHR_present_wait2 can later block until it is on-screen (CPU-side present pacing —
    // the foundation for I6 frames-per-flip). The v1 pair (VK_KHR_present_id/wait) is dropped.
    std::uint64_t thisPresentId = 0;
    const bool tagPresentId = m_ctx != nullptr && m_ctx->presentIdSupported;
    if (tagPresentId) {
        thisPresentId = ++m_presentId;
    }
    VkPresentId2KHR presentIdInfo{
        .sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR,
        .pNext = nullptr,
        .swapchainCount = 1U,
        .pPresentIds = &thisPresentId,
    };

    // MOD4 v1 (maintenance1 fence half): chain a per-slot present fence. It signals when
    // the presentation engine is done with the image; Swapchain::recreate waits it before
    // retiring the old swapchain, so resize no longer needs a whole-device idle. This
    // VkFence is not a MOD2 regression: the presentation engine's completion signal only
    // exists as a fence (VkSwapchainPresentFenceInfoKHR has no timeline form) — all
    // frame-path sync above the present stays on the shared timeline.
    const bool chainPresentFence = m_ctx != nullptr && m_ctx->swapchainMaintenance1Supported;
    VkFence presentFence = VK_NULL_HANDLE;
    if (chainPresentFence) {
        // The slot's previous present fence has signaled by construction: the image can
        // only be re-acquired after the presentation engine released it — which is what
        // the fence signals. The defensive wait keeps the vkResetFences below legal even
        // if that ordering ever slips (e.g. after a failed present on this slot).
        if (m_presentFencePending[imageIndex]) {
            static_cast<void>(
                vkWaitForFences(m_ctx->device, 1, m_presentFences[imageIndex].ptr(), VK_TRUE, kWaitForever));
            m_presentFencePending[imageIndex] = false;
        }
        static_cast<void>(vkResetFences(m_ctx->device, 1, m_presentFences[imageIndex].ptr()));
        presentFence = m_presentFences[imageIndex];
        m_presentFencePending[imageIndex] = true;
    }
    const VkSwapchainPresentFenceInfoKHR presentFenceInfo{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR,
        .pNext = nullptr,
        .swapchainCount = 1U,
        .pFences = &presentFence,
    };
    // Optional present-info pNext chain: present id (MOD6) → present fence (MOD4 v1).
    if (chainPresentFence) {
        presentIdInfo.pNext = &presentFenceInfo;
    }

    const VkPresentInfoKHR presentInfo{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = tagPresentId        ? &presentIdInfo
                 : chainPresentFence ? static_cast<const void*>(&presentFenceInfo)
                                     : nullptr,
        .waitSemaphoreCount = waitSemaphore != VK_NULL_HANDLE ? 1U : 0U,
        .pWaitSemaphores = waitSemaphore != VK_NULL_HANDLE ? &waitSemaphore : nullptr,
        .swapchainCount = 1U,
        .pSwapchains = m_swapchain.ptr(),
        .pImageIndices = &imageIndex,
        .pResults = nullptr,
    };
    const VkResult result = vkQueuePresentKHR(queue, &presentInfo);
    if (chainPresentFence) {
        // A queued present (SUCCESS / SUBOPTIMAL) hands the image to the presentation
        // engine — no longer acquired. A rejected present (e.g. OUT_OF_DATE) leaves the
        // image in the acquired state, so recreate()'s release pass picks it up.
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
            m_acquired[imageIndex] = false;
        }
    }
    return result;
}

VkResult Swapchain::waitForPresent(std::uint64_t presentId, std::uint64_t timeoutNs) {
    if (m_ctx == nullptr || m_swapchain == VK_NULL_HANDLE || !m_ctx->presentWaitSupported) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    // MOD6: vkWaitForPresent2KHR (info-struct form) replaces vkWaitForPresentKHR.
    const VkPresentWait2InfoKHR waitInfo{
        .sType = VK_STRUCTURE_TYPE_PRESENT_WAIT_2_INFO_KHR,
        .pNext = nullptr,
        .presentId = presentId,
        .timeout = timeoutNs,
    };
    return vkWaitForPresent2KHR(m_ctx->device, m_swapchain, &waitInfo);
}

VkResult Swapchain::recreate(VkExtent2D newExtent) {
    if (m_ctx == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // MOD4 v1: quiesce the OLD swapchain before creating the new one — without a
    // whole-device idle (that is the fence/release half's purpose):
    //  1. Bounded wait on the present fences of slots whose present is still queued.
    //     A signaled present fence guarantees the presentation engine is done with the
    //     image AND that the present's wait semaphore (renderComplete) was consumed —
    //     the present completes after it — so retiring the swapchain and later
    //     destroying FrameSync's per-image semaphores is safe.
    //  2. vkReleaseSwapchainImagesKHR for images still acquired (acquired but never
    //     presented — e.g. the frame this resize interrupted).
    // Extension-absent fallback: the legacy pre-recreate device idle (unchanged
    // behavior — the fence path is what removes it on capable devices).
    if (m_ctx->swapchainMaintenance1Supported) {
        quiesceForRecreate();
    } else {
        vkDeviceWaitIdle(m_ctx->device);
    }

    // Pass the old swapchain so the driver can retire it and reuse resources.
    auto recreated = create(*m_ctx, m_surface, newExtent, m_preferHDR, m_swapchain);
    if (!recreated) {
        return recreated.error();
    }

    // Move-assignment retires the old swapchain + views (RAII) and adopts the new one.
    const DeviceContext* savedCtx = recreated->m_ctx;
    *this = std::move(*recreated);
    if (m_ctx == nullptr) {
        m_ctx = savedCtx;
    }
    return VK_SUCCESS;
}

void Swapchain::quiesceForRecreate() {
    const VkDevice device = m_ctx->device;

    // 1. Wait (bounded) every fence still chained to a queued present.
    std::vector<VkFence> pending;
    pending.reserve(m_presentFencePending.size());
    for (std::size_t i = 0; i < m_presentFencePending.size(); ++i) {
        if (m_presentFencePending[i]) {
            pending.push_back(m_presentFences[i]);
        }
    }
    if (!pending.empty()) {
        const VkResult waitResult = vkWaitForFences(
            device, static_cast<std::uint32_t>(pending.size()), pending.data(), VK_TRUE, kPresentFenceTimeoutNs);
        if (waitResult != VK_SUCCESS) {
            // Pathological only (presentation engine stalled / device lost): the idle is
            // the safety net that keeps the retirement below legal — it costs this one
            // resize, not the steady-state path.
            core::Logger::warn("Swapchain recreate: present fence wait failed (VkResult {}) — falling back to "
                               "vkDeviceWaitIdle",
                               static_cast<int>(waitResult));
            vkDeviceWaitIdle(device);
        }
    }

    // 2. Release the images still acquired (never presented) back to the retired
    //    swapchain — the maintenance1 destroy precondition.
    std::vector<std::uint32_t> acquiredIndices;
    acquiredIndices.reserve(m_acquired.size());
    for (std::uint32_t i = 0; i < m_acquired.size(); ++i) {
        if (m_acquired[i]) {
            acquiredIndices.push_back(i);
        }
    }
    if (!acquiredIndices.empty()) {
        const VkReleaseSwapchainImagesInfoKHR releaseInfo{
            .sType = VK_STRUCTURE_TYPE_RELEASE_SWAPCHAIN_IMAGES_INFO_KHR,
            .pNext = nullptr,
            .swapchain = m_swapchain,
            .imageIndexCount = static_cast<std::uint32_t>(acquiredIndices.size()),
            .pImageIndices = acquiredIndices.data(),
        };
        const VkResult releaseResult = vkReleaseSwapchainImagesKHR(device, &releaseInfo);
        if (releaseResult != VK_SUCCESS) {
            core::Logger::warn("Swapchain recreate: vkReleaseSwapchainImagesKHR failed (VkResult {})",
                               static_cast<int>(releaseResult));
        }
    }

    // 3. The swapchain handle is replaced right after this — reset the slot state so
    //    the fresh swapchain starts clean (its fences are new anyway).
    m_presentFencePending.assign(m_presentFencePending.size(), false);
    m_acquired.assign(m_acquired.size(), false);
}

VkSwapchainKHR Swapchain::handle() const noexcept {
    return m_swapchain;
}

VkFormat Swapchain::format() const noexcept {
    return m_format;
}

VkColorSpaceKHR Swapchain::colorSpace() const noexcept {
    return m_colorSpace;
}

VkExtent2D Swapchain::extent() const noexcept {
    return m_extent;
}

std::uint32_t Swapchain::imageCount() const noexcept {
    return static_cast<std::uint32_t>(m_images.size());
}

VkImage Swapchain::image(std::uint32_t i) const noexcept {
    return i < m_images.size() ? m_images[i] : VK_NULL_HANDLE;
}

VkImageView Swapchain::imageView(std::uint32_t i) const noexcept {
    return i < m_views.size() ? m_views[i] : VK_NULL_HANDLE;
}

utils::OutputColorSpace Swapchain::outputColorSpace() const noexcept {
    // Swapchain::create refuses color spaces without a tonemap encoding (H-05
    // seam guard), so the fallback is unreachable for a live swapchain.
    return mapOutputColorSpace(m_colorSpace).value_or(utils::OutputColorSpace::eSDR);
}

std::optional<utils::OutputColorSpace> mapOutputColorSpace(VkColorSpaceKHR colorSpace) noexcept {
    switch (colorSpace) {
    case VK_COLOR_SPACE_HDR10_ST2084_EXT:
        return utils::OutputColorSpace::eHDR10;
    case VK_COLOR_SPACE_HDR10_HLG_EXT:
        return utils::OutputColorSpace::eHLG;
    case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
        return utils::OutputColorSpace::eScRGB;
    case VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT:
        return utils::OutputColorSpace::eDisplayP3;
    case VK_COLOR_SPACE_BT709_LINEAR_EXT:
        return utils::OutputColorSpace::eSDRLinear;
    case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR:
        return utils::OutputColorSpace::eSDR;
    default:
        return std::nullopt;
    }
}

} // namespace harmonia::presentation
