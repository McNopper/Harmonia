#pragma once

#include <volk/volk.h>

#include <slang-math/slang-math.hpp>

#include <expected>
#include <memory>
#include <string>
#include <string_view>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/Buffer.hpp"
#include "harmonia/core/CommandPool.hpp"
#include "harmonia/renderer/AccelerationStructure.hpp"
#include "harmonia/scene/Mesh.hpp"

struct Xform {
    sm::float3 translation = {0.0f, 0.0f, 0.0f};
    sm::quaternion rotation = sm::identity<sm::quaternion>();
    sm::float3 scale = {1.0f, 1.0f, 1.0f};

    [[nodiscard]] sm::float4x4 matrix() const noexcept;
    [[nodiscard]] sm::float4x4 inverseMatrix() const noexcept;
    [[nodiscard]] VkTransformMatrixKHR toVkTransform() const noexcept;
};

class Geometry {
  public:
    Xform xform;
    uint32_t materialIndex = 0;

    virtual ~Geometry() = default;

    virtual VkResult buildBlas(const DeviceContext& ctx, const CommandPool& pool) = 0;
    [[nodiscard]] virtual VkAccelerationStructureInstanceKHR makeInstance(uint32_t instanceIndex) const noexcept = 0;

    [[nodiscard]] VkAccelerationStructureKHR blas() const noexcept { return m_blas; }

  protected:
    VkAccelerationStructureKHR m_blas = VK_NULL_HANDLE;
};

class TriangleMesh final : public Geometry {
  public:
    [[nodiscard]] static std::expected<std::unique_ptr<TriangleMesh>, VkResult> create(const DeviceContext& ctx,
                                                                                       const CommandPool& pool,
                                                                                       MeshData data,
                                                                                       uint32_t materialIndex,
                                                                                       std::string_view debugName = "");

    VkResult buildBlas(const DeviceContext& ctx, const CommandPool& pool) override;
    [[nodiscard]] VkAccelerationStructureInstanceKHR makeInstance(uint32_t instanceIndex) const noexcept override;

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
    [[nodiscard]] static std::expected<std::unique_ptr<Sphere>, VkResult> create(const DeviceContext& ctx,
                                                                                 const CommandPool& pool,
                                                                                 sm::float3 center,
                                                                                 float radius,
                                                                                 uint32_t materialIndex,
                                                                                 std::string_view debugName = "");

    VkResult buildBlas(const DeviceContext& ctx, const CommandPool& pool) override;
    [[nodiscard]] VkAccelerationStructureInstanceKHR makeInstance(uint32_t instanceIndex) const noexcept override;

    [[nodiscard]] sm::float3 center() const noexcept;
    [[nodiscard]] float radius() const noexcept;

  private:
    sm::float3 m_center{};
    float m_radius = 0.0f;
    Buffer m_aabbBuffer{};
    AccelerationStructure m_accelerationStructure{};
    std::string m_debugName{};
};
