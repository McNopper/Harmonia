#ifndef HARMONIA_SCENE_MESH_HPP
#define HARMONIA_SCENE_MESH_HPP

#include <volk/volk.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <slang-math/slang-math.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/GpuTypes.hpp"
#include "harmonia/core/Buffer.hpp"
#include "harmonia/core/CommandPool.hpp"

namespace harmonia {

/// Axis-aligned bounding box (object or world space — caller-established).
struct Aabb {
    sm::float3 min{};
    sm::float3 max{};
};

struct MeshData {
    std::vector<GpuVertex> vertices;
    std::vector<std::uint32_t> indices;
    /// Optional object-space AABB authored in the scene file. When absent the
    /// consumer derives it from `vertices`. Carried to the renderers, which
    /// transform it to world space per instance (`worldAabbFromInstance`).
    std::optional<Aabb> bounds;
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
