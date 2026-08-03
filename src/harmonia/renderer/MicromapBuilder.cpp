#include "harmonia/renderer/MicromapBuilder.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "harmonia/core/Logger.hpp"

namespace harmonia {

namespace {

/// Upload a host byte span to a device-local buffer with device-address +
/// micromap-build-input usage, aligned to 256 bytes (micromap data/triangle
/// device addresses must be 256-aligned — VUID-vkCmdBuildMicromapsEXT-pInfos-07515).
[[nodiscard]] std::expected<Buffer, VkResult> uploadMicromapInput(const DeviceContext& ctx,
                                                                  const CommandPool& pool,
                                                                  std::span<const std::byte> bytes,
                                                                  std::string_view name) {
    constexpr VkDeviceSize kMicromapDataAlignment = 256;
    return Buffer::upload(ctx,
                          pool,
                          bytes,
                          VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT |
                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          name,
                          kMicromapDataAlignment);
}

/// Scratch buffer for a micromap build, sized from the build sizes and aligned
/// to the device's `minAccelerationStructureScratchOffsetAlignment` (the same
/// property AS-build scratch uses — micromap builds share it).
[[nodiscard]] std::expected<Buffer, VkResult>
createMicromapScratch(const DeviceContext& ctx, VkDeviceSize buildScratchSize, std::string_view debugName) {
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{};
    asProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &asProps;
    vkGetPhysicalDeviceProperties2(ctx.physicalDevice, &props);

    const VkDeviceSize padded =
        std::max<VkDeviceSize>(buildScratchSize + asProps.minAccelerationStructureScratchOffsetAlignment, 16);
    return Buffer::create(ctx,
                          padded,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                          debugName);
}

} // namespace

std::expected<Micromap, VkResult> MicromapBuilder::build(const DeviceContext& ctx,
                                                         const CommandPool& pool,
                                                         const aether::OpacityMicromapGroup& group,
                                                         std::string_view debugName) {
    const std::string base = debugName.empty() ? std::string("omm") : std::string(debugName);

    // 1) Packed-state data buffer (the microtriangle opacity bits, LSB-first).
    auto dataBuffer =
        uploadMicromapInput(ctx,
                            pool,
                            std::as_bytes(std::span<const std::byte>(group.dataBits.data(), group.dataBits.size())),
                            base + ".omm.data");
    if (!dataBuffer) {
        return std::unexpected(dataBuffer.error());
    }

    // 2) Per-record triangle array. OpacityMicromapTriangle is layout-compatible
    // with VkMicromapTriangleEXT ({u32 dataOffset, u16 level, u16 format}, 8B).
    static_assert(sizeof(aether::OpacityMicromapTriangle) == sizeof(VkMicromapTriangleEXT),
                  "OpacityMicromapTriangle must match VkMicromapTriangleEXT layout");
    auto triangleBuffer = uploadMicromapInput(
        ctx,
        pool,
        std::as_bytes(std::span<const aether::OpacityMicromapTriangle>(group.triangles.data(), group.triangles.size())),
        base + ".omm.triangles");
    if (!triangleBuffer) {
        return std::unexpected(triangleBuffer.error());
    }

    // 3) Per-base-triangle index buffer (record ordinal, or a special encoded
    // as its two's-complement uint32 bit-pattern — glTF/Vulkan convention).
    // This is consumed by the BLAS build (vkCmdBuildAccelerationStructuresKHR),
    // NOT the micromap build, so it carries the acceleration-structure build-input
    // usage (the micromap-build-input flag is only for the data/triangle buffers).
    std::vector<std::uint32_t> indices{};
    indices.reserve(group.micromapIndices.size());
    for (const std::int32_t idx : group.micromapIndices) {
        indices.push_back(static_cast<std::uint32_t>(idx)); // two's-complement for negatives
    }
    auto indexBuffer = Buffer::upload(ctx,
                                      pool,
                                      std::as_bytes(std::span<const std::uint32_t>(indices.data(), indices.size())),
                                      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                      base + ".omm.indices",
                                      256);
    if (!indexBuffer) {
        return std::unexpected(indexBuffer.error());
    }

    // 4) Host-side usage histogram (VkMicromapUsageEXT is {u32,u32,u32} = 12B —
    // NOT layout-compatible with the Aether struct — so build it here).
    std::vector<VkMicromapUsageEXT> usage{};
    usage.reserve(group.usage.size());
    for (const auto& u : group.usage) {
        usage.push_back(VkMicromapUsageEXT{
            .count = u.count,
            .subdivisionLevel = static_cast<std::uint32_t>(u.subdivisionLevel),
            .format = static_cast<std::uint32_t>(u.format),
        });
    }

    // 5) Size the micromap + scratch from the usage histogram.
    VkMicromapBuildInfoEXT buildInfo{};
    buildInfo.sType = VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT;
    buildInfo.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
    buildInfo.flags = VK_BUILD_MICROMAP_PREFER_FAST_TRACE_BIT_EXT;
    buildInfo.mode = VK_BUILD_MICROMAP_MODE_BUILD_EXT;
    buildInfo.dstMicromap = VK_NULL_HANDLE;
    buildInfo.usageCountsCount = static_cast<std::uint32_t>(usage.size());
    buildInfo.pUsageCounts = usage.data();
    buildInfo.data.deviceAddress = dataBuffer->deviceAddress();
    buildInfo.scratchData.deviceAddress = 0;
    buildInfo.triangleArray.deviceAddress = triangleBuffer->deviceAddress();
    buildInfo.triangleArrayStride = sizeof(VkMicromapTriangleEXT);

    VkMicromapBuildSizesInfoEXT sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT;
    vkGetMicromapBuildSizesEXT(ctx.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &sizes);
    if (sizes.micromapSize == 0U) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    // 6) Storage buffer + VkMicromapEXT handle.
    auto storage = Buffer::create(ctx,
                                  sizes.micromapSize,
                                  VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                  VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                  base + ".omm.storage");
    if (!storage) {
        return std::unexpected(storage.error());
    }

    const VkMicromapCreateInfoEXT createInfo{
        .sType = VK_STRUCTURE_TYPE_MICROMAP_CREATE_INFO_EXT,
        .pNext = nullptr,
        .createFlags = 0,
        .buffer = storage->handle(),
        .offset = 0,
        .size = sizes.micromapSize,
        .type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT,
        .deviceAddress = 0,
    };

    VkMicromapEXT handle{};
    if (const VkResult result = vkCreateMicromapEXT(ctx.device, &createInfo, nullptr, &handle); result != VK_SUCCESS) {
        return std::unexpected(result);
    }

    // 7) Scratch buffer (aligned), then record the build in a one-shot.
    auto scratch = createMicromapScratch(ctx, sizes.buildScratchSize, base + ".omm.scratch");
    if (!scratch) {
        return std::unexpected(scratch.error());
    }
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{};
    asProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &asProps;
    vkGetPhysicalDeviceProperties2(ctx.physicalDevice, &props);
    const VkDeviceAddress scratchAddress =
        bufferAlignUp(scratch->deviceAddress(), asProps.minAccelerationStructureScratchOffsetAlignment);

    buildInfo.dstMicromap = handle;
    buildInfo.scratchData.deviceAddress = scratchAddress;

    auto buildPool = CommandPool::create(ctx, ctx.graphicsFamily);
    if (!buildPool) {
        return std::unexpected(buildPool.error());
    }
    auto cmd = buildPool->beginOneShot();
    if (!cmd) {
        return std::unexpected(cmd.error());
    }
    vkCmdBuildMicromapsEXT(*cmd, 1, &buildInfo);
    if (const VkResult result = buildPool->endOneShot(*cmd); result != VK_SUCCESS) {
        return std::unexpected(result);
    }

    if (!base.empty()) {
        ctx.setDebugName(VK_OBJECT_TYPE_MICROMAP_EXT, handle, base.c_str());
    }

    Micromap out{};
    out.m_dataBuffer = std::move(*dataBuffer);
    out.m_triangleBuffer = std::move(*triangleBuffer);
    out.m_storageBuffer = std::move(*storage);
    out.m_scratchBuffer = std::move(*scratch);
    out.m_indexBuffer = std::move(*indexBuffer);
    out.m_usage = std::move(usage);
    out.m_handle = UniqueMicromapEXT{ctx.device, handle};
    out.m_indexCount = static_cast<std::uint32_t>(indices.size());

    Logger::info("MicromapBuilder '{}': {} base triangles ({} records, {}B data), micromap {}B",
                 base,
                 out.m_indexCount,
                 group.triangles.size(),
                 group.dataBits.size(),
                 sizes.micromapSize);
    return out;
}

} // namespace harmonia
