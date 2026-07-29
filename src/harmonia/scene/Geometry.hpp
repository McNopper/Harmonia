#ifndef HARMONIA_SCENE_GEOMETRY_HPP
#define HARMONIA_SCENE_GEOMETRY_HPP

#include <volk/volk.h>

#include <expected>
#include <memory>
#include <slang-math/slang-math.hpp>
#include <string>
#include <string_view>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/Buffer.hpp"
#include "harmonia/core/CommandPool.hpp"
#include "harmonia/renderer/AccelerationStructure.hpp"
#include "harmonia/scene/Mesh.hpp"

/// Decomposed object→world transform (glTF T × R × S).
///
/// A transform belongs to an *instance*, not a mesh: one mesh can be placed many
/// times with different transforms (true GPU instancing). `toVkTransform()` packs
/// it into the 3×4 matrix a TLAS instance carries.
struct Xform {
    sm::float3 translation = {0.0f, 0.0f, 0.0f};
    sm::quaternion rotation = sm::identity<sm::quaternion>();
    sm::float3 scale = {1.0f, 1.0f, 1.0f};

    [[nodiscard]] sm::float4x4 matrix() const noexcept;
    [[nodiscard]] sm::float4x4 inverseMatrix() const noexcept;
    [[nodiscard]] VkTransformMatrixKHR toVkTransform() const noexcept;
};

/// A placed instance: which unique mesh, with which transform and material.
///
/// Renderers hold one of these per instance; the referenced `Geometry` (the mesh)
/// owns the shared BLAS, so N instances of one mesh share one BLAS.
struct InstanceRecord {
    uint32_t meshIndex = 0;
    Xform xform{};
    uint32_t materialIndex = 0;
};

/// A unique piece of geometry living in **object space**, owning one BLAS.
///
/// Transform and material are per-instance (see `InstanceRecord`), not per-mesh.
/// `makeInstance` emits a TLAS instance referencing this mesh's BLAS with a
/// caller-supplied transform, so many instances can reference one shared BLAS.
class Geometry {
  public:
    virtual ~Geometry() = default;

    virtual VkResult buildBlas(const DeviceContext& ctx, const CommandPool& pool) = 0;
    [[nodiscard]] virtual VkAccelerationStructureInstanceKHR makeInstance(uint32_t instanceCustomIndex,
                                                                          const Xform& xform) const noexcept = 0;

    [[nodiscard]] VkAccelerationStructureKHR blas() const noexcept { return m_blas; }

  protected:
    VkAccelerationStructureKHR m_blas = VK_NULL_HANDLE;
};

class TriangleMesh final : public Geometry {
  public:
    [[nodiscard]] static std::expected<std::unique_ptr<TriangleMesh>, VkResult>
    create(const DeviceContext& ctx, const CommandPool& pool, MeshData&& data, std::string_view debugName = "");

    VkResult buildBlas(const DeviceContext& ctx, const CommandPool& pool) override;
    [[nodiscard]] VkAccelerationStructureInstanceKHR makeInstance(uint32_t instanceCustomIndex,
                                                                  const Xform& xform) const noexcept override;

    [[nodiscard]] const Buffer& vertexBuffer() const noexcept;
    [[nodiscard]] const Buffer& indexBuffer() const noexcept;
    [[nodiscard]] uint32_t vertexCount() const noexcept;
    [[nodiscard]] uint32_t indexCount() const noexcept;
    [[nodiscard]] const MeshData& data() const noexcept;

  private:
    MeshData m_data{};
    Mesh m_mesh{};
    AccelerationStructure m_accelerationStructure{};
    std::string m_debugName{};
};

class Sphere final : public Geometry {
  public:
    /// Analytic sphere of @p radius centred at the object-space origin. Placement
    /// is the instance transform's job (translation/scale via the TLAS).
    [[nodiscard]] static std::expected<std::unique_ptr<Sphere>, VkResult>
    create(const DeviceContext& ctx, const CommandPool& pool, float radius, std::string_view debugName = "");

    VkResult buildBlas(const DeviceContext& ctx, const CommandPool& pool) override;
    [[nodiscard]] VkAccelerationStructureInstanceKHR makeInstance(uint32_t instanceCustomIndex,
                                                                  const Xform& xform) const noexcept override;

    [[nodiscard]] float radius() const noexcept;

  private:
    float m_radius = 0.0f;
    Buffer m_aabbBuffer{};
    AccelerationStructure m_accelerationStructure{};
    std::string m_debugName{};
};
#endif // HARMONIA_SCENE_GEOMETRY_HPP
