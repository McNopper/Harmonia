#include "harmonia/rt/TlasBuilder.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>

#include "harmonia/core/Buffer.hpp"

namespace harmonia::rt {
namespace {

[[nodiscard]] VkBuildAccelerationStructureFlagsKHR tlasFlags(const TlasBuildOptions& options) {
    VkBuildAccelerationStructureFlagsKHR flags = options.preferFastBuild
                                                     ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR
                                                     : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    if (options.allowUpdate) {
        flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    }
    if (options.allowCompaction) {
        flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
    }
    return flags;
}

} // namespace

VkResult buildTlas(const DeviceContext& ctx,
                   const core::CommandPool& pool,
                   std::span<const VkAccelerationStructureInstanceKHR> instances,
                   AccelerationStructure& outTlas,
                   VkDeviceAddress& outAddress) {
    return buildTlas(ctx, pool, instances, TlasBuildOptions{}, outTlas, outAddress);
}

VkResult buildTlas(const DeviceContext& ctx,
                   const core::CommandPool& pool,
                   std::span<const VkAccelerationStructureInstanceKHR> instances,
                   const TlasBuildOptions& options,
                   AccelerationStructure& outTlas,
                   VkDeviceAddress& outAddress) {
    auto instanceUpload = core::Buffer::upload(
        ctx,
        pool,
        std::as_bytes(std::span<const VkAccelerationStructureInstanceKHR>(instances.data(), instances.size())),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
        "scene.tlas.instances");
    if (!instanceUpload) {
        return instanceUpload.error();
    }

    const VkAccelerationStructureGeometryInstancesDataKHR instancesData{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
        .pNext = nullptr,
        .arrayOfPointers = VK_FALSE,
        .data = VkDeviceOrHostAddressConstKHR{instanceUpload->deviceAddress()},
    };
    const VkAccelerationStructureGeometryKHR geometry{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .pNext = nullptr,
        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .geometry = VkAccelerationStructureGeometryDataKHR{.instances = instancesData},
        .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
    };
    const std::uint32_t primitiveCount = static_cast<std::uint32_t>(instances.size());
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .pNext = nullptr,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        .flags = tlasFlags(options),
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

    auto tlasAS = AccelerationStructure::create(
        ctx, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, sizeInfo.accelerationStructureSize, "scene.tlas");
    if (!tlasAS) {
        return tlasAS.error();
    }

    auto scratch = createAccelerationStructureScratch(ctx, sizeInfo, "scene.tlasScratch");
    if (!scratch) {
        return scratch.error();
    }

    buildInfo.dstAccelerationStructure = tlasAS->handle();
    buildInfo.scratchData.deviceAddress = scratch->alignedAddress;
    const VkAccelerationStructureBuildRangeInfoKHR rangeInfo{
        .primitiveCount = primitiveCount,
        .primitiveOffset = 0,
        .firstVertex = 0,
        .transformOffset = 0,
    };
    const VkAccelerationStructureBuildRangeInfoKHR* rangePtr = &rangeInfo;

    auto cmd = pool.beginOneShot();
    if (!cmd) {
        return cmd.error();
    }
    vkCmdBuildAccelerationStructuresKHR(*cmd, 1, &buildInfo, &rangePtr);
    if (const VkResult result = pool.endOneShot(*cmd); result != VK_SUCCESS) {
        return result;
    }

    outTlas = std::move(*tlasAS);
    outAddress = outTlas.deviceAddress();
    return VK_SUCCESS;
}

VkResult refitTlas(const DeviceContext& ctx,
                   const core::CommandPool& pool,
                   std::span<const VkAccelerationStructureInstanceKHR> instances,
                   const TlasBuildOptions& options,
                   AccelerationStructure& tlas) {
    if (tlas.handle() == VK_NULL_HANDLE) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    auto instanceUpload = core::Buffer::upload(
        ctx,
        pool,
        std::as_bytes(std::span<const VkAccelerationStructureInstanceKHR>(instances.data(), instances.size())),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
        "scene.tlas.refitInstances");
    if (!instanceUpload) {
        return instanceUpload.error();
    }

    const VkAccelerationStructureGeometryInstancesDataKHR instancesData{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
        .pNext = nullptr,
        .arrayOfPointers = VK_FALSE,
        .data = VkDeviceOrHostAddressConstKHR{instanceUpload->deviceAddress()},
    };
    const VkAccelerationStructureGeometryKHR geometry{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .pNext = nullptr,
        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .geometry = VkAccelerationStructureGeometryDataKHR{.instances = instancesData},
        .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
    };
    const std::uint32_t primitiveCount = static_cast<std::uint32_t>(instances.size());
    // Update mode (the spec's refit): src = dst = the existing AS. The flags
    // must match the source build's flags exactly (spec contract — hence the
    // options parameter), and the instance count/layout must be unchanged:
    // only transforms may differ.
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .pNext = nullptr,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        .flags = tlasFlags(options),
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR,
        .srcAccelerationStructure = tlas.handle(),
        .dstAccelerationStructure = tlas.handle(),
        .geometryCount = 1,
        .pGeometries = &geometry,
        .ppGeometries = nullptr,
        .scratchData = VkDeviceOrHostAddressKHR{},
    };
    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{};
    sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(
        ctx.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &primitiveCount, &sizeInfo);

    // UPDATE consumes updateScratchSize (not buildScratchSize).
    auto refitSizes = sizeInfo;
    refitSizes.buildScratchSize = sizeInfo.updateScratchSize;
    auto scratch = createAccelerationStructureScratch(ctx, refitSizes, "scene.tlas.refitScratch");
    if (!scratch) {
        return scratch.error();
    }

    buildInfo.scratchData.deviceAddress = scratch->alignedAddress;
    const VkAccelerationStructureBuildRangeInfoKHR rangeInfo{
        .primitiveCount = primitiveCount,
        .primitiveOffset = 0,
        .firstVertex = 0,
        .transformOffset = 0,
    };
    const VkAccelerationStructureBuildRangeInfoKHR* rangePtr = &rangeInfo;

    auto cmd = pool.beginOneShot();
    if (!cmd) {
        return cmd.error();
    }
    vkCmdBuildAccelerationStructuresKHR(*cmd, 1, &buildInfo, &rangePtr);
    return pool.endOneShot(*cmd);
}

} // namespace harmonia::rt
