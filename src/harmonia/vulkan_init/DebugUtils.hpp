#ifndef HARMONIA_VULKAN_INIT_DEBUGUTILS_HPP
#define HARMONIA_VULKAN_INIT_DEBUGUTILS_HPP

#include <volk/volk.h>

#include <expected>

namespace harmonia::vulkan_init {

class DebugUtils {
  public:
    [[nodiscard]] static std::expected<DebugUtils, VkResult> create(VkInstance instance);

    /// VK16: dedicated messenger recording VK_EXT_device_address_binding_report binding
    /// events (INFO severity, DEVICE_ADDRESS_BINDING message type only). The events make
    /// the addresses in a VK_EXT_device_fault report correlate-able to named resources.
    [[nodiscard]] static std::expected<DebugUtils, VkResult> createAddressBinding(VkInstance instance);

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

    static VKAPI_ATTR VkBool32 VKAPI_CALL
    addressBindingCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                           VkDebugUtilsMessageTypeFlagsEXT types,
                           const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
                           void* userData);

  private:
    void destroy() noexcept;

    VkInstance m_instance{};
    VkDebugUtilsMessengerEXT m_messenger{};
};

} // namespace harmonia::vulkan_init

#endif // HARMONIA_VULKAN_INIT_DEBUGUTILS_HPP
