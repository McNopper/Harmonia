#ifndef HARMONIA_APP_APPDETAIL_HPP
#define HARMONIA_APP_APPDETAIL_HPP

// Detail shared between the harmonia::app::App translation units
// (App.cpp, AppOffscreen.cpp, AppPresent.cpp). Not public API - do not
// include from outside the app layer.

#include <volk/volk.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>

namespace harmonia::app {

/// Drawable size in pixels (high-DPI aware) - the swapchain extent source.
[[nodiscard]] inline VkExtent2D windowPixelExtent(SDL_Window* window) {
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(window, &width, &height);
    return VkExtent2D{
        .width = static_cast<std::uint32_t>(std::max(width, 1)),
        .height = static_cast<std::uint32_t>(std::max(height, 1)),
    };
}

} // namespace harmonia::app

#endif // HARMONIA_APP_APPDETAIL_HPP
