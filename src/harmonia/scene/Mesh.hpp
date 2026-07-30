#ifndef HARMONIA_SCENE_MESH_HPP
#define HARMONIA_SCENE_MESH_HPP

#include <volk/volk.h>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/GpuTypes.hpp"
#include "harmonia/core/Buffer.hpp"
#include "harmonia/core/CommandPool.hpp"

namespace harmonia {

struct MeshData {
    std::vector<GpuVertex> vertices;
    std::vector<std::uint32_t> indices;
};

class Mesh {
  public:
    Mesh() = default;
    Mesh(Mesh&&) noexcept = default;
    Mesh& operator=(Mesh&&) noexcept = default;
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    ~Mesh() = default;

    [[nodiscard]] static std::expected<Mesh, VkResult>
    create(const DeviceContext& ctx, const CommandPool& cmdPool, const MeshData& data, std::string_view debugName = "");

    [[nodiscard]] const Buffer& vertexBuffer() const noexcept { return m_vertexBuffer; }
    [[nodiscard]] const Buffer& indexBuffer() const noexcept { return m_indexBuffer; }
    [[nodiscard]] std::uint32_t vertexCount() const noexcept { return m_vertexCount; }
    [[nodiscard]] std::uint32_t indexCount() const noexcept { return m_indexCount; }

  private:
    Buffer m_vertexBuffer{};
    Buffer m_indexBuffer{};
    std::uint32_t m_vertexCount{};
    std::uint32_t m_indexCount{};
};

} // namespace harmonia

#endif // HARMONIA_SCENE_MESH_HPP
