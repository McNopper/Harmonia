#ifndef HARMONIA_CORE_SHADERMODULE_HPP
#define HARMONIA_CORE_SHADERMODULE_HPP

#include <volk/volk.h>

#include <expected>
#include <filesystem>
#include <vector>

namespace harmonia {

/// Reads a SPIR-V binary from disk. Pass an absolute path — shader binaries
/// are compiled at build time by compile_slang_shaders (CompileShaders.cmake)
/// into the directory exposed by the project's *_SHADER_DIR compile
/// definition (HARMONIA_SHADER_DIR / HYPERION_SHADER_DIR / THEIA_SHADER_DIR),
/// never loaded relative to the working directory.
[[nodiscard]] std::expected<std::vector<uint32_t>, VkResult> readSpirv(const std::filesystem::path& path);

/// Reads a SPIR-V binary and wraps it in a VkShaderModule.
/// Failures are logged with the offending path.
[[nodiscard]] std::expected<VkShaderModule, VkResult> createShaderModule(VkDevice device,
                                                                         const std::filesystem::path& path);

} // namespace harmonia
#endif // HARMONIA_CORE_SHADERMODULE_HPP
