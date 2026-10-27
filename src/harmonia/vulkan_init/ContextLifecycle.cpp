#include <volk/volk.h>

#include <utility>
#include <vma/vk_mem_alloc.h>

#include "harmonia/vulkan_init/Context.hpp"

namespace harmonia::vulkan_init {

Context::Context(Context&& other) noexcept
    : m_instance(std::exchange(other.m_instance, VK_NULL_HANDLE)),
      m_surface(std::exchange(other.m_surface, VK_NULL_HANDLE)),
      m_debugUtils(std::move(other.m_debugUtils)),
      m_addressBinding(std::move(other.m_addressBinding)),
      m_physicalDeviceInfo(other.m_physicalDeviceInfo),
      m_deviceContext(other.m_deviceContext),
      m_validationEnabled(other.m_validationEnabled) {
    other.m_physicalDeviceInfo = {};
    other.m_deviceContext = {};
    other.m_validationEnabled = false;
}

Context& Context::operator=(Context&& other) noexcept {
    if (this != &other) {
        destroy();
        m_instance = std::exchange(other.m_instance, VK_NULL_HANDLE);
        m_surface = std::exchange(other.m_surface, VK_NULL_HANDLE);
        m_debugUtils = std::move(other.m_debugUtils);
        m_addressBinding = std::move(other.m_addressBinding);
        m_physicalDeviceInfo = other.m_physicalDeviceInfo;
        m_deviceContext = other.m_deviceContext;
        m_validationEnabled = other.m_validationEnabled;
        other.m_physicalDeviceInfo = {};
        other.m_deviceContext = {};
        other.m_validationEnabled = false;
    }
    return *this;
}

Context::~Context() {
    destroy();
}

const DeviceContext& Context::deviceContext() const noexcept {
    return m_deviceContext;
}

VkInstance Context::instance() const noexcept {
    return m_instance;
}

VkSurfaceKHR Context::surface() const noexcept {
    return m_surface;
}

const PhysicalDeviceInfo& Context::physicalDeviceInfo() const noexcept {
    return m_physicalDeviceInfo;
}

void Context::destroy() noexcept {
    if (m_deviceContext.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_deviceContext.device);
    }
    if (m_deviceContext.allocator != VK_NULL_HANDLE) {
        vmaDestroyAllocator(m_deviceContext.allocator);
    }
    if (m_deviceContext.device != VK_NULL_HANDLE) {
        vkDestroyDevice(m_deviceContext.device, nullptr);
    }
    if (m_instance != VK_NULL_HANDLE && m_surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    }

    m_deviceContext = {};
    m_physicalDeviceInfo = {};
    m_surface = VK_NULL_HANDLE;
    m_debugUtils = {};
    m_addressBinding = {};

    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
    }
    m_instance = VK_NULL_HANDLE;
    m_validationEnabled = false;
}

} // namespace harmonia::vulkan_init
