#ifndef HARMONIA_APP_FRAMESYNC_HPP
#define HARMONIA_APP_FRAMESYNC_HPP

#include <volk/volk.h>

#include <array>
#include <cstdint>
#include <vector>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/CommandPool.hpp"
#include "harmonia/core/VulkanHandle.hpp"

namespace harmonia {

/// Frame synchronisation for the App render loop (R8/CH9): owns the double-buffered frame
/// slots (per-slot command buffers + acquire semaphore + timeline completion value), the
/// per-swapchain-image render-complete semaphores, the shared timeline semaphore, and the
/// swapchain-image layout tracking. Exposes the timeline/slot/rotation primitives so App's
/// frame-loop methods (record / acquire / submit / present) delegate the sync mechanics here
/// and keep only the rendering orchestration.
class FrameSync {
  public:
    static constexpr std::uint32_t kFrameSlots = 2;

    FrameSync() = default;
    ~FrameSync() = default;
    FrameSync(const FrameSync&) = delete;
    FrameSync& operator=(const FrameSync&) = delete;
    FrameSync(FrameSync&&) noexcept = default;
    FrameSync& operator=(FrameSync&&) noexcept = default;

    /// Allocate per-slot command buffers + acquire semaphores, per-image render-complete
    /// semaphores, and the timeline semaphore. Returns false on allocation failure.
    [[nodiscard]] bool create(const DeviceContext& ctx, CommandPool& cmdPool, std::uint32_t swapchainImageCount);
    /// Release all owned sync resources. (Command buffers are pool-owned and not freed here.)
    void destroy() noexcept;
    /// Re-create per-image render-complete semaphores for a new swapchain image count and
    /// reset slot state. Call after vkDeviceWaitIdle() + swapchain recreate.
    void onResize(std::uint32_t swapchainImageCount) noexcept;

    // ── frame-slot / timeline primitives ─────────────────────────────────────
    [[nodiscard]] std::uint32_t currentSlot() const noexcept { return m_currentFrame; }
    [[nodiscard]] VkCommandBuffer renderCmd(std::uint32_t slot) const noexcept { return m_frames[slot].renderCmd; }
    [[nodiscard]] VkCommandBuffer displayCmd(std::uint32_t slot) const noexcept { return m_frames[slot].displayCmd; }
    /// Block until the previous submission on this slot is done (timeline wait on its
    /// completion value). No-op when the slot is idle (completion value 0).
    void waitSlotComplete(std::uint32_t slot, std::uint64_t timeoutNs = UINT64_MAX) const noexcept;
    /// Allocate the next monotonic timeline signal value.
    [[nodiscard]] std::uint64_t nextSignalValue() noexcept { return m_nextTimelineValue++; }
    /// Record that this slot's latest submission signals @p value.
    void markSlotComplete(std::uint32_t slot, std::uint64_t value) noexcept { m_frames[slot].completionValue = value; }
    /// Advance to the next frame slot (round-robin).
    void rotate() noexcept { m_currentFrame = (m_currentFrame + 1U) % kFrameSlots; }
    /// Reset per-slot command buffers (to INITIAL state) + completion values to 0.
    void resetSlots() noexcept;

    // ── semaphore accessors (for VkSubmitInfo2 / VkPresentInfo / acquire) ─────
    [[nodiscard]] VkSemaphore imageAvailable(std::uint32_t slot) const noexcept { return m_frames[slot].imageAvailable; }
    [[nodiscard]] VkSemaphore renderComplete(std::uint32_t imageIndex) const noexcept { return m_renderComplete[imageIndex]; }
    [[nodiscard]] VkSemaphore timeline() const noexcept { return m_timelineSemaphore; }

    // ── swapchain-image layout tracking (present barriers) ────────────────────
    [[nodiscard]] VkImageLayout swapchainLayout(std::uint32_t imageIndex) const noexcept { return m_swapchainLayouts[imageIndex]; }
    void setSwapchainLayout(std::uint32_t imageIndex, VkImageLayout layout) noexcept { m_swapchainLayouts[imageIndex] = layout; }

  private:
    struct FrameResources {
        VkCommandBuffer renderCmd{};
        VkCommandBuffer displayCmd{};
        harmonia::UniqueSemaphore imageAvailable;
        std::uint64_t completionValue{};
    };

    VkDevice m_device{};
    std::array<FrameResources, kFrameSlots> m_frames{};
    std::vector<harmonia::UniqueSemaphore> m_renderComplete;
    harmonia::UniqueSemaphore m_timelineSemaphore;
    std::uint64_t m_nextTimelineValue = 1;
    std::uint32_t m_currentFrame = 0;
    std::vector<VkImageLayout> m_swapchainLayouts;
};

} // namespace harmonia

#endif // HARMONIA_APP_FRAMESYNC_HPP
