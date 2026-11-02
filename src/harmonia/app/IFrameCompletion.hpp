#ifndef HARMONIA_APP_IFRAMECOMPLETION_HPP
#define HARMONIA_APP_IFRAMECOMPLETION_HPP

#include <expected>
#include <thread>

#include "harmonia/core/CommandPool.hpp"

namespace harmonia::app {

/// A producer's bounded GPU work for ONE logical frame. The host may observe
/// only completion/error status, never ray counts or dispatch dimensions. Status
/// is queried only after the preceding submission has finished; implementations
/// must invalidate non-coherent readback memory before reading it.
class IFrameCompletion {
  public:
    virtual ~IFrameCompletion() = default;
    [[nodiscard]] virtual std::expected<bool, VkResult> ready() noexcept = 0;
    /// Resume existing work only: do not launch a new frame/sample or accumulate
    /// an incomplete estimate. All workgroup counts remain GPU-generated.
    [[nodiscard]] virtual VkResult recordContinuation(VkCommandBuffer cmd) noexcept = 0;
};

/// Caller has already submitted AND waited for the initial producer work. No
/// fixed trial/iteration cap: a pause is resumed, while errors abort the frame.
/// Each continuation must be individually bounded; frame/sample IDs stay fixed.
[[nodiscard]] inline VkResult drainFrameWork(IFrameCompletion& work, const core::CommandPool& pool) noexcept {
    for (;;) {
        const auto ready = work.ready();
        if (!ready)
            return ready.error();
        if (*ready)
            return VK_SUCCESS;
        const auto cmd = pool.beginOneShot();
        if (!cmd)
            return cmd.error();
        const auto recorded = work.recordContinuation(*cmd);
        if (recorded != VK_SUCCESS) {
            pool.free(*cmd);
            return recorded;
        }
        const auto submitted = pool.endOneShot(*cmd);
        if (submitted != VK_SUCCESS)
            return submitted;
        std::this_thread::yield();
    }
}

} // namespace harmonia::app
#endif // HARMONIA_APP_IFRAMECOMPLETION_HPP
