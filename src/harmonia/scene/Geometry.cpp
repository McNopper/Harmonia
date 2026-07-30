#include "harmonia/scene/Geometry.hpp"

#include <volk/volk.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <slang-math/slang-math.hpp>
#include <span>
#include <string>
#include <utility>

#include "harmonia/GpuTypes.hpp"
#include "harmonia/scene/ProceduralGeometry.hpp"

namespace harmonia {

namespace {
[[nodiscard]] VkResult buildSingleBlas(const DeviceContext& ctx,
                                       const CommandPool& fallbackPool,
                                       std::string_view debugName,
                                       VkAccelerationStructureGeometryKHR& geometry,
                                       VkAccelerationStructureBuildRangeInfoKHR& rangeInfo,
                                       AccelerationStructure& outBlas,
                                       VkAccelerationStructureKHR& outHandle) {
    if (outBlas.handle() != VK_NULL_HANDLE) {
        outHandle = outBlas.handle();
        return VK_SUCCESS;
    }

    const std::uint32_t primitiveCount = rangeInfo.primitiveCount;
    VkBuildAccelerationStructureFlagsKHR flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    if (ctx.positionFetchSupported) {
        flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DATA_ACCESS_KHR;
    }
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .pNext = nullptr,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        .flags = flags,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .srcAccelerationStructure = VK_NULL_HANDLE,
        .dstAccelerationStructure = VK_NULL_HANDLE,
        .geometryCount = 1,
        .pGeometries = &geometry,
        .ppGeometries = nullptr,
        .scratchData = VkDeviceOrHostAddressKHR{},
    };

    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{};
    sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(
        ctx.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &primitiveCount, &sizeInfo);

    auto blas = AccelerationStructure::create(ctx,
                                              VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                                              sizeInfo.accelerationStructureSize,
                                              std::string(debugName).append(".blas"));
    if (!blas) {
        return blas.error();
    }

    auto scratch = createAccelerationStructureScratch(ctx, sizeInfo, std::string(debugName).append(".scratch"));
    if (!scratch) {
        return scratch.error();
    }

    auto buildPool = CommandPool::create(ctx, ctx.graphicsFamily);
    if (!buildPool) {
        return buildPool.error();
    }
    const CommandPool& commandPool = *buildPool;
    (void)fallbackPool;

    buildInfo.dstAccelerationStructure = blas->handle();
    buildInfo.scratchData.deviceAddress = scratch->alignedAddress;

    const VkAccelerationStructureBuildRangeInfoKHR* rangeInfoPtr = &rangeInfo;
    auto cmd = commandPool.beginOneShot();
    if (!cmd) {
        return cmd.error();
    }

    vkCmdBuildAccelerationStructuresKHR(*cmd, 1, &buildInfo, &rangeInfoPtr);
    if (const VkResult result = commandPool.endOneShot(*cmd); result != VK_SUCCESS) {
        return result;
    }

    outBlas = std::move(*blas);
    outHandle = outBlas.handle();
    return VK_SUCCESS;
}
} // namespace

sm::float4x4 Xform::matrix() const noexcept {
    return sm::translate(sm::float4x4(1.0f), translation) * sm::toFloat4x4(rotation) *
           sm::scale(sm::float4x4(1.0f), scale);
}

sm::float4x4 Xform::inverseMatrix() const noexcept {
    return sm::inverse(matrix());
}

VkTransformMatrixKHR Xform::toVkTransform() const noexcept {
    const sm::float4x4 transform = matrix();
    return VkTransformMatrixKHR{{
        {transform[0][0], transform[0][1], transform[0][2], transform[0][3]},
        {transform[1][0], transform[1][1], transform[1][2], transform[1][3]},
        {transform[2][0], transform[2][1], transform[2][2], transform[2][3]},
    }};
}

std::expected<std::unique_ptr<TriangleMesh>, VkResult>
TriangleMesh::create(const DeviceContext& ctx, const CommandPool& pool, MeshData&& data, std::string_view debugName) {
    auto mesh = Mesh::create(ctx, pool, data, debugName);
    if (!mesh) {
        return std::unexpected(mesh.error());
    }

    auto triangleMesh = std::make_unique<TriangleMesh>();
    triangleMesh->m_data = std::move(data);
    triangleMesh->m_mesh = std::move(*mesh);
    triangleMesh->m_debugName = debugName.empty() ? "triangle_mesh" : std::string(debugName);
    return triangleMesh;
}

VkResult TriangleMesh::buildBlas(const DeviceContext& ctx, const CommandPool& pool) {
    const std::uint32_t primitiveCount = m_mesh.indexCount() / 3;
    const std::uint32_t maxVertex = m_mesh.vertexCount() == 0 ? 0U : m_mesh.vertexCount() - 1U;

    VkAccelerationStructureGeometryKHR geometry{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .pNext = nullptr,
        .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
        .geometry =
            VkAccelerationStructureGeometryDataKHR{
                .triangles =
                    VkAccelerationStructureGeometryTrianglesDataKHR{
                        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                        .pNext = nullptr,
                        .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
                        .vertexData = VkDeviceOrHostAddressConstKHR{m_mesh.vertexBuffer().deviceAddress()},
                        .vertexStride = sizeof(GpuVertex),
                        .maxVertex = maxVertex,
                        .indexType = VK_INDEX_TYPE_UINT32,
                        .indexData = VkDeviceOrHostAddressConstKHR{m_mesh.indexBuffer().deviceAddress()},
                        .transformData = VkDeviceOrHostAddressConstKHR{},
                    },
            },
        .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
    };

    VkAccelerationStructureBuildRangeInfoKHR rangeInfo{
        .primitiveCount = primitiveCount,
        .primitiveOffset = 0,
        .firstVertex = 0,
        .transformOffset = 0,
    };

    return buildSingleBlas(ctx, pool, m_debugName, geometry, rangeInfo, m_accelerationStructure, m_blas);
}

VkAccelerationStructureInstanceKHR TriangleMesh::makeInstance(std::uint32_t instanceCustomIndex,
                                                              const Xform& xform) const noexcept {
    return VkAccelerationStructureInstanceKHR{
        .transform = xform.toVkTransform(),
        .instanceCustomIndex = instanceCustomIndex,
        .mask = kInstanceMaskAll,
        .instanceShaderBindingTableRecordOffset = 0,
        .flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR,
        .accelerationStructureReference = m_accelerationStructure.deviceAddress(),
    };
}

const Buffer& TriangleMesh::vertexBuffer() const noexcept {
    return m_mesh.vertexBuffer();
}

const Buffer& TriangleMesh::indexBuffer() const noexcept {
    return m_mesh.indexBuffer();
}

std::uint32_t TriangleMesh::vertexCount() const noexcept {
    return m_mesh.vertexCount();
}

std::uint32_t TriangleMesh::indexCount() const noexcept {
    return m_mesh.indexCount();
}

const MeshData& TriangleMesh::data() const noexcept {
    return m_data;
}

std::expected<std::unique_ptr<Sphere>, VkResult>
Sphere::create(const DeviceContext& ctx, const CommandPool& pool, float radius, std::string_view debugName) {
    if (radius <= 0.0f) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    // Object-space AABB centred at the origin; placement (translation/scale) is
    // the instance transform's job via the TLAS.
    const auto aabb = harmonia::ProceduralGeometry::makeSphereAabb(sm::float3{0.0f, 0.0f, 0.0f}, radius);
    const VkAabbPositionsKHR vkAabb{
        .minX = aabb.min.x,
        .minY = aabb.min.y,
        .minZ = aabb.min.z,
        .maxX = aabb.max.x,
        .maxY = aabb.max.y,
        .maxZ = aabb.max.z,
    };

    auto aabbBuffer = Buffer::upload(ctx,
                                     pool,
                                     std::as_bytes(std::span<const VkAabbPositionsKHR>(&vkAabb, 1)),
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                         VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                                     debugName.empty() ? "sphere.aabb" : std::string(debugName).append(".aabb"));
    if (!aabbBuffer) {
        return std::unexpected(aabbBuffer.error());
    }

    auto sphere = std::make_unique<Sphere>();
    sphere->m_radius = radius;
    sphere->m_aabbBuffer = std::move(*aabbBuffer);
    sphere->m_debugName = debugName.empty() ? "sphere" : std::string(debugName);
    return sphere;
}

VkResult Sphere::buildBlas(const DeviceContext& ctx, const CommandPool& pool) {
    VkAccelerationStructureGeometryKHR geometry{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .pNext = nullptr,
        .geometryType = VK_GEOMETRY_TYPE_AABBS_KHR,
        .geometry =
            VkAccelerationStructureGeometryDataKHR{
                .aabbs =
                    VkAccelerationStructureGeometryAabbsDataKHR{
                        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR,
                        .pNext = nullptr,
                        .data = VkDeviceOrHostAddressConstKHR{m_aabbBuffer.deviceAddress()},
                        .stride = sizeof(VkAabbPositionsKHR),
                    },
            },
        .flags = VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR,
    };

    VkAccelerationStructureBuildRangeInfoKHR rangeInfo{
        .primitiveCount = 1,
        .primitiveOffset = 0,
        .firstVertex = 0,
        .transformOffset = 0,
    };

    return buildSingleBlas(ctx, pool, m_debugName, geometry, rangeInfo, m_accelerationStructure, m_blas);
}

VkAccelerationStructureInstanceKHR Sphere::makeInstance(std::uint32_t instanceCustomIndex,
                                                        const Xform& xform) const noexcept {
    return VkAccelerationStructureInstanceKHR{
        .transform = xform.toVkTransform(),
        .instanceCustomIndex = instanceCustomIndex,
        .mask = kInstanceMaskAll,
        .instanceShaderBindingTableRecordOffset = 1,
        .flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR,
        .accelerationStructureReference = m_accelerationStructure.deviceAddress(),
    };
}

float Sphere::radius() const noexcept {
    return m_radius;
}

} // namespace harmonia
