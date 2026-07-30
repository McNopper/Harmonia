#ifndef HARMONIA_VULKAN_INIT_DEBUGUTILS_HPP
#define HARMONIA_VULKAN_INIT_DEBUGUTILS_HPP

#include <volk/volk.h>

#include <expected>

namespace harmonia {

class DebugUtils {
  public:
    [[nodiscard]] static std::expected<DebugUtils, VkResult> create(VkInstance instance);

    /// Canonical messenger create-info (severity/type mask + Harmonia callback).
    /// Shared by instance-creation pNext chaining and post-instance messenger setup.
    [[nodiscard]] static VkDebugUtilsMessengerCreateInfoEXT messengerCreateInfo() noexcept;

    DebugUtils() = default;
    DebugUtils(const DebugUtils&) = delete;
    DebugUtils& operator=(const DebugUtils&) = delete;
    DebugUtils(DebugUtils&& other) noexcept;
    DebugUtils& operator=(DebugUtils&& other) noexcept;
    ~DebugUtils();

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                        VkDebugUtilsMessageTypeFlagsEXT types,
                                                        const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
                                                        void* userData);

  private:
    void destroy() noexcept;

    VkInstance m_instance{};
    VkDebugUtilsMessengerEXT m_messenger{};
};

} // namespace harmonia

#endif // HARMONIA_VULKAN_INIT_DEBUGUTILS_HPP
