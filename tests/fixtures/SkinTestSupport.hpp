#ifndef HARMONIA_TESTS_FIXTURES_SKINTESTSUPPORT_HPP
#define HARMONIA_TESTS_FIXTURES_SKINTESTSUPPORT_HPP

// ANI4 shared test support: a miniature renderer-half SceneBase + the quad-skin
// scene fixtures. The stub is deliberately the same shape as BOTH real
// renderer halves (scene-global vertex buffer built by buildSceneBuffers +
// per-mesh ranges into it), so the SkinDeformPass integration tested here is
// the wiring the renderers ship.

#include <volk/volk.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "aether/format/ObjImporter.hpp"
#include "harmonia/GpuTypes.hpp"
#include "harmonia/scene/SceneBase.hpp"
#include "harmonia/scene_rt/SceneRt.hpp"
#include "harmonia/scene_rt/TriangleMesh.hpp"

namespace harmonia::test {

/// Minimal concrete SceneBase: triangle meshes through TriangleMesh, no
/// spheres, TLAS through scene_rt - plus the renderer-half global vertex
/// buffer (concatenated GpuVertex + per-mesh offsets) the skinning pass needs.
class StubScene final : public scene::SceneBase {
  public:
    std::uint32_t addSphereMesh(const DeviceContext&, const core::CommandPool&, float, std::string_view) override {
        return std::numeric_limits<std::uint32_t>::max(); // fixtures never place spheres
    }

    /// The scene-global vertex buffer (renderer-layout miniature).
    [[nodiscard]] const core::Buffer& sceneVertices() const noexcept { return m_sceneVertices; }
    /// Immutable rest-pose copy (the skinning kernel's read source).
    [[nodiscard]] const core::Buffer& vertexRestBuffer() const noexcept { return m_vertexRestBuffer; }
    /// Per-mesh range start in the scene-global buffer, indexed by mesh index.
    [[nodiscard]] std::span<const std::uint32_t> meshSceneVertexOffsets() const noexcept { return m_meshSceneOffsets; }

  private:
    VkResult buildSceneBuffers(const DeviceContext& ctx, const core::CommandPool& pool) override {
        std::vector<GpuVertex> vertices;
        m_meshSceneOffsets.assign(m_meshes.size(), 0U);
        for (std::size_t mi = 0; mi < m_meshes.size(); ++mi) {
            const auto* mesh = dynamic_cast<const scene_rt::TriangleMesh*>(m_meshes[mi].get());
            if (mesh == nullptr) {
                continue;
            }
            m_meshSceneOffsets[mi] = static_cast<std::uint32_t>(vertices.size());
            vertices.insert(vertices.end(), mesh->data().vertices.begin(), mesh->data().vertices.end());
        }
        if (vertices.empty()) {
            vertices.push_back(GpuVertex{});
        }
        constexpr VkBufferUsageFlags kVertexStorage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        auto buf = uploadStorageBuffer(ctx,
                                       pool,
                                       std::as_bytes(std::span<const GpuVertex>(vertices.data(), vertices.size())),
                                       "test.scene.vertices",
                                       kVertexStorage);
        if (!buf) {
            return buf.error();
        }
        m_sceneVertices = std::move(*buf);
        // ANI4: skinned scenes also keep an immutable rest copy (the kernel's
        // read source; the active buffer holds the previous frame's pose).
        if (hasSkins() || hasMorphs()) {
            auto rest = uploadStorageBuffer(ctx,
                                            pool,
                                            std::as_bytes(std::span<const GpuVertex>(vertices.data(), vertices.size())),
                                            "test.scene.vertexRest",
                                            kVertexStorage);
            if (!rest) {
                return rest.error();
            }
            m_vertexRestBuffer = std::move(*rest);
        }
        return VK_SUCCESS;
    }
    std::unique_ptr<scene::Geometry> createTriangleMesh(const DeviceContext& ctx,
                                                        const core::CommandPool& pool,
                                                        scene::MeshData&& data,
                                                        const scene::MeshOpacity& opacity,
                                                        std::string_view name) override {
        auto mesh = scene_rt::TriangleMesh::create(ctx, pool, std::move(data), opacity, name);
        if (!mesh) {
            return nullptr;
        }
        return std::move(*mesh);
    }
    VkResult finishBuild(const DeviceContext& ctx, const core::CommandPool& pool) override {
        return scene_rt::buildTlas(*this, ctx, pool, m_tlas, m_tlasAddress);
    }

    core::Buffer m_sceneVertices{};
    core::Buffer m_vertexRestBuffer{};
    std::vector<std::uint32_t> m_meshSceneOffsets;
    rt::AccelerationStructure m_tlas{};
    VkDeviceAddress m_tlasAddress = 0;
};

/// Mini OBJ: a unit quad (2 triangles, full v/vt/vn face refs) - 4
/// deduplicated vertices, the count a [[skin]]'s per-vertex arrays must match.
inline void writeQuadObj(const std::filesystem::path& path) {
    std::ofstream out(path);
    out << "v 0 0 0\n"
           "v 1 0 0\n"
           "v 1 1 0\n"
           "v 0 1 0\n"
           "vt 0 0\n"
           "vt 1 0\n"
           "vt 1 1\n"
           "vt 0 1\n"
           "vn 0 0 1\n"
           "f 1/1/1 2/2/1 3/3/1\n"
           "f 1/1/1 3/3/1 4/4/1\n";
}

inline std::size_t quadVertexCount(const std::filesystem::path& path) {
    const auto groups = aether::ObjImporter::parse(path);
    if (!groups) {
        return 0;
    }
    std::size_t total = 0;
    for (const aether::MeshGroup& g : *groups) {
        total += g.mesh.vertices.size();
    }
    return total;
}

/// Skin scene over the quad: one root joint + the meshed part node. When
/// @p animate, an [[animation]] track drives the joint's translation (the
/// poses must then follow the tick clock).
inline std::string skinSceneToml(std::size_t vertexCount, bool animate) {
    std::ostringstream os;
    if (animate) {
        os << "[animation_clock]\nfps = 30\n\n";
    }
    os << "[[mesh]]\n"
          "name = \"q\"\n"
          "path = \"q.obj\"\n"
          "\n"
          "[[node]]\n"
          "id = \"root\"\n"
          "\n"
          "[[node]]\n"
          "id = \"part\"\n"
          "mesh = \"q\"\n"
          "\n"
          "[[skin]]\n"
          "node = \"part\"\n"
          "joints = [\"root\"]\n"
          "inverse_bind = [[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]]\n"
          "bone_indices = [";
    for (std::size_t v = 0; v < vertexCount; ++v) {
        os << (v == 0 ? "\n  " : ",\n  ") << "[0, 0, 0, 0]";
    }
    os << "\n]\nbone_weights = [";
    for (std::size_t v = 0; v < vertexCount; ++v) {
        os << (v == 0 ? "\n  " : ",\n  ") << "[1, 0, 0, 0]";
    }
    os << "\n]\n";
    if (animate) {
        os << "\n[[animation]]\n"
              "target = \"nodes/root/translation\"\n"
              "interpolation = \"linear\"\n"
              "times = [0.0, 1.0]\n"
              "values = [0, 0, 0, 3, 0, 0]\n";
    }
    return os.str();
}

struct SkinFixture {
    std::filesystem::path dir;
    std::size_t vertexCount = 0;

    SkinFixture(std::string_view name, bool animate) {
        namespace fs = std::filesystem;
        dir = fs::temp_directory_path() / ("harmonia_skin_rt_" + std::string{name});
        fs::create_directories(dir);
        writeQuadObj(dir / "q.obj");
        vertexCount = quadVertexCount(dir / "q.obj");
        std::ofstream scn(dir / "s.scene.toml");
        scn << skinSceneToml(vertexCount, animate);
    }
    ~SkinFixture() { std::filesystem::remove_all(dir); }

    [[nodiscard]] std::filesystem::path scenePath() const { return dir / "s.scene.toml"; }
};

} // namespace harmonia::test
#endif // HARMONIA_TESTS_FIXTURES_SKINTESTSUPPORT_HPP
