#include "harmonia/core/ShaderModule.hpp"

#include <cstdint>
#include <fstream>

#include "harmonia/core/Logger.hpp"
#include "harmonia/core/ShaderModule.hpp"

namespace harmonia {

std::expected<std::vector<std::uint32_t>, VkResult> readSpirv(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        Logger::error("Failed to open SPIR-V file: {}", path.string());
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    const auto size = file.tellg();
    if (size <= 0 || (static_cast<std::size_t>(size) % sizeof(std::uint32_t)) != 0U) {
        Logger::error("Invalid SPIR-V file size for: {}", path.string());
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    std::vector<std::uint32_t> code(static_cast<std::size_t>(size) / sizeof(std::uint32_t));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(code.data()), size);
    if (!file) {
        Logger::error("Failed to read SPIR-V file: {}", path.string());
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    return code;
}

std::expected<VkShaderModule, VkResult> createShaderModule(VkDevice device, const std::filesystem::path& path) {
    auto code = readSpirv(path);
    if (!code) {
        return std::unexpected(code.error());
    }

    const VkShaderModuleCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = code->size() * sizeof(std::uint32_t),
        .pCode = code->data(),
    };

    VkShaderModule module = VK_NULL_HANDLE;
    if (const VkResult result = vkCreateShaderModule(device, &createInfo, nullptr, &module); result != VK_SUCCESS) {
        Logger::error("Failed to create shader module from: {}", path.string());
        return std::unexpected(result);
    }
    return module;
}

} // namespace harmonia
