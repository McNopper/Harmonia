#ifndef HARMONIA_PRESENTATION_SWAPCHAIN_HPP
#define HARMONIA_PRESENTATION_SWAPCHAIN_HPP

#include <volk/volk.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <vector>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/VulkanHandle.hpp"
#include "harmonia/utils/OutputColorSpace.hpp"

namespace harmonia::presentation {

/// Map a Vulkan surface color space to the display-referred OutputColorSpace
/// the ToneMapper encodes for.
///
/// H-05 seam guard: the shared ToneMapper ships an encoding per OutputColorSpace
/// and nothing else. A surface color space outside that set (e.g. Display-P3
/// LINEAR) has no correct tonemap encoding — presenting it through the sRGB path
/// would silently apply the wrong transfer function, so the swapchain creation
/// refuses it (@ref Swapchain::create) instead of defaulting. Returns nullopt
/// for unmapped color spaces.
[[nodiscard]] std::optional<utils::OutputColorSpace> mapOutputColorSpace(VkColorSpaceKHR colorSpace) noexcept;

class Swapchain {
  public:
    [[nodiscard]] static std::expected<Swapchain, VkResult> create(const DeviceContext& ctx,
                                                                   VkSurfaceKHR surface,
                                                                   VkExtent2D extent,
                                                                   bool preferHDR = true,
                                                                   VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE);

    Swapchain() = default;
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;
    Swapchain(Swapchain&&) noexcept = default;
    Swapchain& operator=(Swapchain&&) noexcept = default;
    ~Swapchain() = default;

    VkResult acquireNextImage(VkSemaphore signalSemaphore, std::uint32_t& outIndex);
    VkResult present(VkQueue queue, std::uint32_t imageIndex, VkSemaphore waitSemaphore);
    /// Block (up to @p timeoutNs) until the present tagged with @p presentId is on-screen.
    /// Requires VK_KHR_present_wait. Returns VK_SUCCESS when displayed, VK_TIMEOUT on expiry.
    VkResult waitForPresent(std::uint64_t presentId, std::uint64_t timeoutNs = UINT64_MAX);
    VkResult recreate(VkExtent2D newExtent);

    [[nodiscard]] VkSwapchainKHR handle() const noexcept;
    [[nodiscard]] VkFormat format() const noexcept;
    [[nodiscard]] VkColorSpaceKHR colorSpace() const noexcept;
    [[nodiscard]] VkExtent2D extent() const noexcept;
    [[nodiscard]] std::uint32_t imageCount() const noexcept;
    [[nodiscard]] VkImage image(std::uint32_t i) const noexcept;
    [[nodiscard]] VkImageView imageView(std::uint32_t i) const noexcept;

    /// Returns the utils::OutputColorSpace that was negotiated with the display.
    /// Use this to drive the ToneMapper each frame.
    [[nodiscard]] utils::OutputColorSpace outputColorSpace() const noexcept;

  private:
    const DeviceContext* m_ctx{};
    VkSurfaceKHR m_surface{};
    VkPhysicalDevice m_physicalDevice{};
    harmonia::core::UniqueSwapchainKHR m_swapchain;
    VkFormat m_format{VK_FORMAT_UNDEFINED};
    VkColorSpaceKHR m_colorSpace{VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    VkExtent2D m_extent{};
    bool m_preferHDR = true;
    std::uint64_t m_presentId = 0; ///< Monotonic per-present ID for VK_KHR_present_id / present_wait.
    std::vector<VkImage> m_images;
    std::vector<harmonia::core::UniqueImageView> m_views;
    /// MOD4 v1 (VK_KHR_swapchain_maintenance1 fence/release half): one present fence per
    /// swapchain image slot, recreated with the swapchain. The fence signals when the
    /// presentation engine is done with that image — what lets @ref recreate retire the
    /// old swapchain without a whole-device idle. Empty when the extension is absent.
    std::vector<harmonia::core::UniqueFence> m_presentFences;
    /// Per-slot bookkeeping backing the fence/release path (sized with the fences):
    /// fence chained to a queued present and not yet waited / image acquired from the
    /// presentation engine and not (yet) presented.
    std::vector<bool> m_presentFencePending;
    std::vector<bool> m_acquired;

    /// Quiesce the live swapchain before recreation: bounded wait on the pending
    /// present fences, then release still-acquired images. No device idle — that is
    /// the fence/release half's purpose. Call only with the extension enabled.
    void quiesceForRecreate();
};

} // namespace harmonia::presentation

#endif // HARMONIA_PRESENTATION_SWAPCHAIN_HPP
