#ifndef HARMONIA_PRESENTATION_SWAPCHAIN_HPP
#define HARMONIA_PRESENTATION_SWAPCHAIN_HPP

#include <volk/volk.h>

#include <cstdint>
#include <expected>
#include <vector>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/VulkanHandle.hpp"
#include "harmonia/presentation/OutputColorSpace.hpp"

namespace harmonia {

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
    VkResult recreate(VkExtent2D newExtent);

    [[nodiscard]] VkSwapchainKHR handle() const noexcept;
    [[nodiscard]] VkFormat format() const noexcept;
    [[nodiscard]] VkColorSpaceKHR colorSpace() const noexcept;
    [[nodiscard]] VkExtent2D extent() const noexcept;
    [[nodiscard]] std::uint32_t imageCount() const noexcept;
    [[nodiscard]] VkImage image(std::uint32_t i) const noexcept;
    [[nodiscard]] VkImageView imageView(std::uint32_t i) const noexcept;

    /// Returns the OutputColorSpace that was negotiated with the display.
    /// Use this to drive the ToneMapper each frame.
    [[nodiscard]] OutputColorSpace outputColorSpace() const noexcept;

  private:
    const DeviceContext* m_ctx{};
    VkSurfaceKHR m_surface{};
    VkPhysicalDevice m_physicalDevice{};
    harmonia::UniqueSwapchainKHR m_swapchain;
    VkFormat m_format{VK_FORMAT_UNDEFINED};
    VkColorSpaceKHR m_colorSpace{VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    VkExtent2D m_extent{};
    bool m_preferHDR = true;
    std::vector<VkImage> m_images;
    std::vector<harmonia::UniqueImageView> m_views;
};

} // namespace harmonia

#endif // HARMONIA_PRESENTATION_SWAPCHAIN_HPP
