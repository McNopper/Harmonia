#include "harmonia/rt/AccelerationStructure.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

#include "harmonia/core/CommandPool.hpp"

namespace harmonia::rt {

std::expected<AccelerationStructure, VkResult> AccelerationStructure::create(const DeviceContext& ctx,
                                                                             VkAccelerationStructureTypeKHR type,
                                                                             VkDeviceSize size,
                                                                             std::string_view debugName) {
    auto storage = core::Buffer::create(ctx,
                                        size,
                                        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                        std::string(debugName).append(".buffer"));
    if (!storage) {
        return std::unexpected(storage.error());
    }

    // MOD5: device-address-only AS creation via vkCreateAccelerationStructure2KHR
    // (VK_KHR_device_address_commands — the forward path per the Khronos AS deprecation
    // blog). The legacy vkCreateAccelerationStructureKHR (buffer-handle + offset + size)
    // is dropped; the address is already known from the backing buffer.
    const VkAccelerationStructureCreateInfo2KHR createInfo{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_2_KHR,
        .pNext = nullptr,
        .createFlags = 0,
        .addressRange =
            {
                .address = storage->deviceAddress(),
                .size = size,
            },
        .addressFlags = 0,
        .type = type,
    };

    AccelerationStructure accelerationStructure;
    accelerationStructure.m_buffer = std::move(*storage);
    // The device address IS the input — no post-creation address query needed.
    accelerationStructure.m_deviceAddress = accelerationStructure.m_buffer.deviceAddress();
    accelerationStructure.m_type = type;

    VkAccelerationStructureKHR handle{};
    if (const VkResult result = vkCreateAccelerationStructure2KHR(ctx.device, &createInfo, nullptr, &handle);
        result != VK_SUCCESS) {
        return std::unexpected(result);
    }
    accelerationStructure.m_handle = harmonia::core::UniqueAccelerationStructure{ctx.device, handle};

    if (!debugName.empty()) {
        ctx.setDebugName(VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR,
                         accelerationStructure.m_handle.get(),
                         std::string(debugName).c_str());
    }

    return accelerationStructure;
}

std::expected<AccelerationStructure, VkResult>
compactAccelerationStructure(const DeviceContext& ctx, const AccelerationStructure& src, std::string_view debugName) {
    // 1. Compact-mode copy needs the compacted size up front: an
    //    ACCELERATION_STRUCTURE_COMPACTED_SIZE query recorded against the
    //    source AS, submitted + waited synchronously (build-time utility).
    VkQueryPool queryPool = VK_NULL_HANDLE;
    const VkQueryPoolCreateInfo queryInfo{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                          .pNext = nullptr,
                                          .flags = 0,
                                          .queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
                                          .queryCount = 1,
                                          .pipelineStatistics = 0};
    if (vkCreateQueryPool(ctx.device, &queryInfo, nullptr, &queryPool) != VK_SUCCESS) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    // The compaction query needs its own short-lived pool (buildSingleBlas
    // precedent: a local CommandPool scoped to the utility).
    auto buildPool = core::CommandPool::create(ctx, ctx.graphicsFamily);
    if (!buildPool) {
        vkDestroyQueryPool(ctx.device, queryPool, nullptr);
        return std::unexpected(buildPool.error());
    }
    const core::CommandPool& pool = *buildPool;
    auto cmd = pool.beginOneShot();
    if (!cmd) {
        vkDestroyQueryPool(ctx.device, queryPool, nullptr);
        return std::unexpected(cmd.error());
    }
    vkCmdResetQueryPool(*cmd, queryPool, 0, 1);
    const VkAccelerationStructureKHR queried = src.handle();
    vkCmdWriteAccelerationStructuresPropertiesKHR(
        *cmd, 1U, &queried, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, queryPool, 0U);
    if (const VkResult result = pool.endOneShot(*cmd); result != VK_SUCCESS) {
        vkDestroyQueryPool(ctx.device, queryPool, nullptr);
        return std::unexpected(result);
    }

    VkDeviceSize compactedSize = 0;
    if (vkGetQueryPoolResults(ctx.device,
                              queryPool,
                              0,
                              1,
                              sizeof(compactedSize),
                              &compactedSize,
                              sizeof(compactedSize),
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) {
        vkDestroyQueryPool(ctx.device, queryPool, nullptr);
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }
    vkDestroyQueryPool(ctx.device, queryPool, nullptr);
    if (compactedSize == 0) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    // 2. Allocate the compacted AS (same type), then record the compact copy
    //    DEVICE-SIDE (vkCmdCopyAccelerationStructureKHR — the Khronos-sanctioned
    //    replacement for the deprecated host vkCopyAccelerationStructureKHR;
    //    the host path would additionally require accelerationStructureHostCommands,
    //    host-visible AS storage and legacy vkCreateAccelerationStructureKHR — all
    //    contrary to this repo's device-address-only AS path, MOD5).
    auto compact = AccelerationStructure::create(ctx, src.type(), compactedSize, debugName);
    if (!compact) {
        return std::unexpected(compact.error());
    }
    auto copyCmd = pool.beginOneShot();
    if (!copyCmd) {
        return std::unexpected(copyCmd.error());
    }
    const VkCopyAccelerationStructureInfoKHR copyInfo{.sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR,
                                                      .pNext = nullptr,
                                                      .src = src.handle(),
                                                      .dst = compact->handle(),
                                                      .mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR};
    vkCmdCopyAccelerationStructureKHR(*copyCmd, &copyInfo);
    if (const VkResult result = pool.endOneShot(*copyCmd); result != VK_SUCCESS) {
        return std::unexpected(result);
    }
    return compact;
}

std::expected<AccelerationStructureScratch, VkResult>
createAccelerationStructureScratch(const DeviceContext& ctx,
                                   const VkAccelerationStructureBuildSizesInfoKHR& sizes,
                                   std::string_view debugName) {
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{};
    asProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &asProps;
    vkGetPhysicalDeviceProperties2(ctx.physicalDevice, &props);

    auto scratch = core::Buffer::create(
        ctx,
        std::max<VkDeviceSize>(sizes.buildScratchSize + asProps.minAccelerationStructureScratchOffsetAlignment, 16),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        debugName);
    if (!scratch) {
        return std::unexpected(scratch.error());
    }

    AccelerationStructureScratch out;
    out.buffer = std::move(*scratch);
    out.alignedAddress =
        core::bufferAlignUp(out.buffer.deviceAddress(), asProps.minAccelerationStructureScratchOffsetAlignment);
    return out;
}

} // namespace harmonia::rt
