#include "harmonia/vulkan_init/DebugUtils.hpp"

#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "harmonia/core/Logger.hpp"

namespace harmonia::vulkan_init {

namespace {
[[nodiscard]] const char* severityLabel(VkDebugUtilsMessageSeverityFlagBitsEXT severity) noexcept {
    switch (severity) {
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
        return "warning";
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
        return "error";
    default:
        return "message";
    }
}

[[nodiscard]] const char* typeLabel(VkDebugUtilsMessageTypeFlagsEXT types) noexcept {
    if ((types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0U) {
        return "validation";
    }
    if ((types & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) != 0U) {
        return "performance";
    }
    return "general";
}

/// Human-readable label for the object types address-binding events report. The
/// common GPU-memory-bound types get names; anything else falls back to the raw enum.
[[nodiscard]] const char* bindingObjectTypeLabel(VkObjectType type) noexcept {
    switch (type) {
    case VK_OBJECT_TYPE_BUFFER:
        return "buffer";
    case VK_OBJECT_TYPE_IMAGE:
        return "image";
    case VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR:
        return "acceleration-structure-khr";
    case VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_NV:
        return "acceleration-structure-nv";
    case VK_OBJECT_TYPE_MICROMAP_EXT:
        return "micromap-ext";
    case VK_OBJECT_TYPE_DEVICE_MEMORY:
        return "device-memory";
    default:
        return nullptr;
    }
}
} // namespace

std::expected<DebugUtils, VkResult> DebugUtils::create(VkInstance instance) {
    if (instance == VK_NULL_HANDLE || vkCreateDebugUtilsMessengerEXT == nullptr) {
        return std::unexpected(VK_ERROR_EXTENSION_NOT_PRESENT);
    }

    const VkDebugUtilsMessengerCreateInfoEXT createInfo = messengerCreateInfo();

    DebugUtils debugUtils;
    debugUtils.m_instance = instance;
    const VkResult result = vkCreateDebugUtilsMessengerEXT(instance, &createInfo, nullptr, &debugUtils.m_messenger);
    if (result != VK_SUCCESS) {
        return std::unexpected(result);
    }

    return debugUtils;
}

std::expected<DebugUtils, VkResult> DebugUtils::createAddressBinding(VkInstance instance) {
    if (instance == VK_NULL_HANDLE || vkCreateDebugUtilsMessengerEXT == nullptr) {
        return std::unexpected(VK_ERROR_EXTENSION_NOT_PRESENT);
    }

    // VK16: binding events arrive as INFO-severity debug-utils messages of the
    // dedicated DEVICE_ADDRESS_BINDING type — nothing else is subscribed here, so
    // this messenger stays silent except for address bind/unbind records.
    const VkDebugUtilsMessengerCreateInfoEXT createInfo{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .pNext = nullptr,
        .flags = 0,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT,
        .pfnUserCallback = &DebugUtils::addressBindingCallback,
        .pUserData = nullptr,
    };

    DebugUtils debugUtils;
    debugUtils.m_instance = instance;
    const VkResult result = vkCreateDebugUtilsMessengerEXT(instance, &createInfo, nullptr, &debugUtils.m_messenger);
    if (result != VK_SUCCESS) {
        return std::unexpected(result);
    }

    return debugUtils;
}

DebugUtils::DebugUtils(DebugUtils&& other) noexcept
    : m_instance(std::exchange(other.m_instance, VK_NULL_HANDLE)),
      m_messenger(std::exchange(other.m_messenger, VK_NULL_HANDLE)) {}

DebugUtils& DebugUtils::operator=(DebugUtils&& other) noexcept {
    if (this != &other) {
        destroy();
        m_instance = std::exchange(other.m_instance, VK_NULL_HANDLE);
        m_messenger = std::exchange(other.m_messenger, VK_NULL_HANDLE);
    }
    return *this;
}

DebugUtils::~DebugUtils() {
    destroy();
}

VkDebugUtilsMessengerCreateInfoEXT DebugUtils::messengerCreateInfo() noexcept {
    return VkDebugUtilsMessengerCreateInfoEXT{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .pNext = nullptr,
        .flags = 0,
        .messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = &DebugUtils::debugCallback,
        .pUserData = nullptr,
    };
}

VKAPI_ATTR VkBool32 VKAPI_CALL DebugUtils::debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                         VkDebugUtilsMessageTypeFlagsEXT types,
                                                         const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
                                                         void* /*userData*/) {
    const std::string_view message = callbackData != nullptr && callbackData->pMessage != nullptr
                                         ? callbackData->pMessage
                                         : std::string_view{"(no message)"};

    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        core::Logger::error("Vulkan {} {}: {}", severityLabel(severity), typeLabel(types), message);
    } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        core::Logger::warn("Vulkan {} {}: {}", severityLabel(severity), typeLabel(types), message);
    }
    return VK_FALSE;
}

VKAPI_ATTR VkBool32 VKAPI_CALL
DebugUtils::addressBindingCallback(VkDebugUtilsMessageSeverityFlagBitsEXT /*severity*/,
                                   VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                   const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
                                   void* /*userData*/) {
    if (callbackData == nullptr) {
        return VK_FALSE;
    }

    // VK16: pull VkDeviceAddressBindingCallbackDataEXT out of the pNext chain.
    const VkDeviceAddressBindingCallbackDataEXT* binding = nullptr;
    for (const VkBaseOutStructure* node = static_cast<const VkBaseOutStructure*>(callbackData->pNext); node != nullptr;
         node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT) {
            binding = reinterpret_cast<const VkDeviceAddressBindingCallbackDataEXT*>(node);
            break;
        }
    }
    if (binding == nullptr) {
        return VK_FALSE;
    }

    // Named resources make forensics actionable: append the debug names the events
    // reference (present when the resources carry vkSetDebugUtilsObjectNameEXT names).
    std::string objects;
    for (std::uint32_t i = 0; callbackData->pObjects != nullptr && i < callbackData->objectCount; ++i) {
        const VkDebugUtilsObjectNameInfoEXT& object = callbackData->pObjects[i];
        const char* label = bindingObjectTypeLabel(object.objectType);
        const std::string typeText =
            label != nullptr ? std::string{label} : std::format("objectType {}", static_cast<int>(object.objectType));
        if (object.pObjectName != nullptr && object.pObjectName[0] != '\0') {
            std::format_to(std::back_inserter(objects),
                           " {} '{}' (handle 0x{:016X})",
                           typeText,
                           object.pObjectName,
                           static_cast<std::uint64_t>(object.objectHandle));
        } else {
            std::format_to(std::back_inserter(objects),
                           " {} (handle 0x{:016X})",
                           typeText,
                           static_cast<std::uint64_t>(object.objectHandle));
        }
    }

    core::Logger::info("AddressBinding: {} baseAddress 0x{:016X} size {:#x}{}{}",
                       binding->bindingType == VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT ? "bind" : "unbind",
                       static_cast<std::uint64_t>(binding->baseAddress),
                       static_cast<std::uint64_t>(binding->size),
                       objects,
                       (binding->flags & VK_DEVICE_ADDRESS_BINDING_INTERNAL_OBJECT_BIT_EXT) != 0U ? " (internal)" : "");
    return VK_FALSE;
}

void DebugUtils::destroy() noexcept {
    if (m_instance != VK_NULL_HANDLE && m_messenger != VK_NULL_HANDLE && vkDestroyDebugUtilsMessengerEXT != nullptr) {
        vkDestroyDebugUtilsMessengerEXT(m_instance, m_messenger, nullptr);
    }
    m_instance = VK_NULL_HANDLE;
    m_messenger = VK_NULL_HANDLE;
}

} // namespace harmonia::vulkan_init
