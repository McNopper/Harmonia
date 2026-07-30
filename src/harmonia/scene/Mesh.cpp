#include "harmonia/scene/Mesh.hpp"

#include <volk/volk.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vma/vk_mem_alloc.h>

namespace harmonia {

std::expected<Mesh, VkResult>
Mesh::create(const DeviceContext& ctx, const CommandPool& cmdPool, const MeshData& data, std::string_view debugName) {
    if (data.vertices.empty() || data.indices.empty()) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    const auto vertexBytes = std::as_bytes(std::span<const GpuVertex>(data.vertices.data(), data.vertices.size()));
    const auto indexBytes = std::as_bytes(std::span<const std::uint32_t>(data.indices.data(), data.indices.size()));

    const std::string baseName = debugName.empty() ? std::string{"mesh"} : std::string{debugName};
    auto vertexBuffer = Buffer::upload(ctx,
                                       cmdPool,
                                       vertexBytes,
                                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                           VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                                       baseName + ".vertices");
    if (!vertexBuffer) {
        return std::unexpected(vertexBuffer.error());
    }

    auto indexBuffer = Buffer::upload(ctx,
                                      cmdPool,
                                      indexBytes,
                                      VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                                      baseName + ".indices");
    if (!indexBuffer) {
        return std::unexpected(indexBuffer.error());
    }

    Mesh mesh;
    mesh.m_vertexBuffer = std::move(*vertexBuffer);
    mesh.m_indexBuffer = std::move(*indexBuffer);
    mesh.m_vertexCount = static_cast<std::uint32_t>(data.vertices.size());
    mesh.m_indexCount = static_cast<std::uint32_t>(data.indices.size());
    return mesh;
}

} // namespace harmonia
