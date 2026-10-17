// GeometryRt.cpp - the ray-tracing state of the scene geometry: BLAS builds for
// TriangleMesh and Sphere (incl. the opacity-micromap bake) and the TLAS
// instance emission. Implements the harmonia::scene::Geometry interface; the
// pure geometry data lives in harmonia/scene (TU split across the scene_rt
// module boundary, no behavior change).
#include <volk/volk.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include "harmonia/GpuTypes.hpp"
#include "harmonia/core/Logger.hpp"
#include "harmonia/scene/ProceduralGeometry.hpp"
#include "harmonia/scene_rt/Sphere.hpp"
#include "harmonia/scene_rt/TriangleMesh.hpp"

namespace harmonia::scene_rt {

using namespace harmonia::scene; // the moved definitions speak scene vocabulary
[[nodiscard]] VkResult buildSingleBlas(const DeviceContext& ctx,
                                       const core::CommandPool& fallbackPool,
                                       std::string_view debugName,
                                       VkAccelerationStructureGeometryKHR& geometry,
                                       VkAccelerationStructureBuildRangeInfoKHR& rangeInfo,
                                       rt::AccelerationStructure& outBlas,
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

    auto blas = rt::AccelerationStructure::create(ctx,
                                                  VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                                                  sizeInfo.accelerationStructureSize,
                                                  std::string(debugName).append(".blas"));
    if (!blas) {
        return blas.error();
    }

    auto scratch = rt::createAccelerationStructureScratch(ctx, sizeInfo, std::string(debugName).append(".scratch"));
    if (!scratch) {
        return scratch.error();
    }

    auto buildPool = core::CommandPool::create(ctx, ctx.graphicsFamily);
    if (!buildPool) {
        return buildPool.error();
    }
    const core::CommandPool& commandPool = *buildPool;
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

std::expected<std::unique_ptr<TriangleMesh>, VkResult> TriangleMesh::create(const DeviceContext& ctx,
                                                                            const core::CommandPool& pool,
                                                                            MeshData&& data,
                                                                            const MeshOpacity& opacity,
                                                                            std::string_view debugName) {
    auto mesh = Mesh::create(ctx, pool, data, debugName);
    if (!mesh) {
        return std::unexpected(mesh.error());
    }

    auto triangleMesh = std::make_unique<TriangleMesh>();
    triangleMesh->m_data = std::move(data);
    triangleMesh->m_mesh = std::move(*mesh);
    triangleMesh->m_opacity = opacity;
    triangleMesh->m_debugName = debugName.empty() ? "triangle_mesh" : std::string(debugName);
    return triangleMesh;
}

VkResult TriangleMesh::buildBlas(const DeviceContext& ctx, const core::CommandPool& pool) {
    const std::uint32_t primitiveCount = m_mesh.indexCount() / 3;
    const std::uint32_t maxVertex = m_mesh.vertexCount() == 0 ? 0U : m_mesh.vertexCount() - 1U;

    // Bake the opacity micromap (if authored) before building the BLAS — the OMM
    // handle + index buffer chain into the triangles descriptor.
    if (m_opacity.micromap != nullptr && !m_micromap.has_value()) {
        auto built = rt::MicromapBuilder::build(ctx, pool, *m_opacity.micromap, m_debugName);
        if (!built) {
            core::Logger::error("TriangleMesh '{}': opacity-micromap build failed (VkResult {})",
                                m_debugName,
                                static_cast<int>(built.error()));
            return built.error();
        }
        if (built->indexCount() != primitiveCount) {
            // One micromap index per BLAS primitive, in the same order — the bake
            // walks the OBJ group's triangles exactly as the importer does. A
            // mismatch means the cached OMM is stale against the mesh.
            core::Logger::error("TriangleMesh '{}': opacity micromap has {} indices but the BLAS has {} triangles "
                                "(stale bake — re-run tools/omm_bake.py)",
                                m_debugName,
                                built->indexCount(),
                                primitiveCount);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        m_micromap = std::move(*built);
    }

    // OMM linkage (lives through the synchronous one-shot build below).
    VkAccelerationStructureTrianglesOpacityMicromapKHR ommLink{};
    VkAccelerationStructureGeometryTrianglesDataKHR triangles{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
        .pNext = nullptr,
        .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
        .vertexData = VkDeviceOrHostAddressConstKHR{m_mesh.vertexBuffer().deviceAddress()},
        .vertexStride = sizeof(GpuVertex),
        .maxVertex = maxVertex,
        .indexType = VK_INDEX_TYPE_UINT32,
        .indexData = VkDeviceOrHostAddressConstKHR{m_mesh.indexBuffer().deviceAddress()},
        .transformData = VkDeviceOrHostAddressConstKHR{},
    };

    // A surface that can be cut out must be non-opaque so the shadow any-hit runs
    // (Hyperion) / the hit surfaces as a RayQuery candidate (Theia); that is what
    // makes `geometry_opacity` attenuate shadow rays. Fully-present geometry keeps
    // the opaque flag, so nothing but cutout meshes pays for it.
    VkGeometryFlagsKHR geometryFlags = m_opacity.alphaTested ? 0 : VK_GEOMETRY_OPAQUE_BIT_KHR;
    if (m_micromap.has_value()) {
        ommLink.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_TRIANGLES_OPACITY_MICROMAP_KHR;
        ommLink.indexType = VK_INDEX_TYPE_UINT32;
        ommLink.indexBuffer = m_micromap->indexBufferAddress();
        ommLink.indexStride = sizeof(std::uint32_t);
        ommLink.baseTriangle = 0;
        ommLink.micromap = m_micromap->handle();
        triangles.pNext = &ommLink;
        geometryFlags = 0;
    }

    VkAccelerationStructureGeometryKHR geometry{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .pNext = nullptr,
        .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
        .geometry = VkAccelerationStructureGeometryDataKHR{.triangles = triangles},
        .flags = geometryFlags,
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
                                                              const Transform& transform) const noexcept {
    return VkAccelerationStructureInstanceKHR{
        .transform = transform.toVkTransform(),
        .instanceCustomIndex = instanceCustomIndex,
        .mask = kInstanceMaskAll,
        .instanceShaderBindingTableRecordOffset = 0,
        .flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR,
        .accelerationStructureReference = m_accelerationStructure.deviceAddress(),
    };
}

const core::Buffer& TriangleMesh::vertexBuffer() const noexcept {
    return m_mesh.vertexBuffer();
}

const core::Buffer& TriangleMesh::indexBuffer() const noexcept {
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

Aabb TriangleMesh::objectAabb() const noexcept {
    if (m_data.bounds) {
        return *m_data.bounds;
    }
    if (m_data.vertices.empty()) {
        return {};
    }
    sm::float3 mn = m_data.vertices[0].position;
    sm::float3 mx = mn;
    for (const GpuVertex& v : m_data.vertices) {
        mn = sm::min(mn, v.position);
        mx = sm::max(mx, v.position);
    }
    return Aabb{.min = mn, .max = mx};
}

std::expected<std::unique_ptr<Sphere>, VkResult>
Sphere::create(const DeviceContext& ctx, const core::CommandPool& pool, float radius, std::string_view debugName) {
    if (radius <= 0.0f) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    // Object-space AABB centred at the origin; placement (translation/scale) is
    // the instance transform's job via the TLAS.
    const auto aabb = harmonia::scene::ProceduralGeometry::makeSphereAabb(sm::float3{0.0f, 0.0f, 0.0f}, radius);
    const VkAabbPositionsKHR vkAabb{
        .minX = aabb.min.x,
        .minY = aabb.min.y,
        .minZ = aabb.min.z,
        .maxX = aabb.max.x,
        .maxY = aabb.max.y,
        .maxZ = aabb.max.z,
    };

    auto aabbBuffer = core::Buffer::upload(ctx,
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

VkResult Sphere::buildBlas(const DeviceContext& ctx, const core::CommandPool& pool) {
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
                                                        const Transform& transform) const noexcept {
    return VkAccelerationStructureInstanceKHR{
        .transform = transform.toVkTransform(),
        .instanceCustomIndex = instanceCustomIndex,
        .mask = kInstanceMaskAll,
        // Two hit-group records per geometry kind (radiance, shadow); procedural
        // geometry is the second pair. See renderer::Pipeline::kHitGroupCount.
        .instanceShaderBindingTableRecordOffset = 2,
        .flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR,
        .accelerationStructureReference = m_accelerationStructure.deviceAddress(),
    };
}

float Sphere::radius() const noexcept {
    return m_radius;
}

Aabb Sphere::objectAabb() const noexcept {
    const auto aabb = harmonia::scene::ProceduralGeometry::makeSphereAabb(sm::float3{0.0f, 0.0f, 0.0f}, m_radius);
    return Aabb{.min = aabb.min, .max = aabb.max};
}

} // namespace harmonia::scene_rt
