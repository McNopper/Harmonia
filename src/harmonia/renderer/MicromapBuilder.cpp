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
/// acceleration-structure-build-input usage, aligned to 256 bytes (micromap
/// data/triangleArray device addresses must be 256-aligned).
[[nodiscard]] std::expected<Buffer, VkResult> uploadMicromapInput(const DeviceContext& ctx,
                                                                  const CommandPool& pool,
                                                                  std::span<const std::byte> bytes,
                                                                  std::string_view name) {
    constexpr VkDeviceSize kMicromapDataAlignment = 256;
    return Buffer::upload(ctx,
                          pool,
                          bytes,
                          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          name,
                          kMicromapDataAlignment);
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
    // with VkMicromapTriangleKHR ({u32 dataOffset, u16 level, u16 format}, 8B).
    static_assert(sizeof(aether::OpacityMicromapTriangle) == sizeof(VkMicromapTriangleKHR),
                  "OpacityMicromapTriangle must match VkMicromapTriangleKHR layout");
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
    // Consumed by the BLAS build through
    // VkAccelerationStructureTrianglesOpacityMicromapKHR::indexBuffer.
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

    // 4) Host-side usage histogram (VkMicromapUsageKHR is {u32,u32,u32} = 12B —
    // NOT layout-compatible with the Aether struct — so build it here).
    std::vector<VkMicromapUsageKHR> usage{};
    usage.reserve(group.usage.size());
    for (const auto& u : group.usage) {
        usage.push_back(VkMicromapUsageKHR{
            .count = u.count,
            .subdivisionLevel = static_cast<std::uint32_t>(u.subdivisionLevel),
            .format = static_cast<VkOpacityMicromapFormatKHR>(u.format),
        });
    }

    // 5) VK_KHR_opacity_micromap: micromaps ARE acceleration structures (the
    // extension proposal's issue 6) — the build input is one
    // VK_GEOMETRY_TYPE_MICROMAP_KHR geometry whose pNext carries data +
    // triangle array + usage histogram.
    VkAccelerationStructureGeometryMicromapDataKHR micromapData{};
    micromapData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_MICROMAP_DATA_KHR;
    micromapData.usageCountsCount = static_cast<std::uint32_t>(usage.size());
    micromapData.pUsageCounts = usage.data();
    micromapData.ppUsageCounts = nullptr;
    micromapData.data = dataBuffer->deviceAddress();
    micromapData.triangleArray = triangleBuffer->deviceAddress();
    micromapData.triangleArrayStride = sizeof(VkMicromapTriangleKHR);

    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.pNext = &micromapData;
    geometry.geometryType = VK_GEOMETRY_TYPE_MICROMAP_KHR;

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;

    // Micromap builds are usage-driven — pMaxPrimitiveCounts must be NULL for
    // VK_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_KHR.
    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(
        ctx.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, nullptr, &sizes);
    if (sizes.accelerationStructureSize == 0U) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    // 6) Micromap object + scratch. MOD5: device-address-only creation via
    // vkCreateAccelerationStructure2KHR — the micromap reuses the exact same path.
    auto micromap = AccelerationStructure::create(
        ctx, VK_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_KHR, sizes.accelerationStructureSize, base + ".omm");
    if (!micromap) {
        return std::unexpected(micromap.error());
    }
    auto scratch = createAccelerationStructureScratch(ctx, sizes, base + ".omm.scratch");
    if (!scratch) {
        return std::unexpected(scratch.error());
    }

    // 7) Record the device-side build in a one-shot (no host builds). Micromap
    // geometry is usage-driven: each ppBuildRangeInfos entry must be NULL for
    // VK_GEOMETRY_TYPE_MICROMAP_KHR (no per-geometry primitive counts).
    buildInfo.dstAccelerationStructure = micromap->handle();
    buildInfo.scratchData.deviceAddress = scratch->alignedAddress;

    const VkAccelerationStructureBuildRangeInfoKHR* rangeInfos[] = {nullptr};

    auto buildPool = CommandPool::create(ctx, ctx.graphicsFamily);
    if (!buildPool) {
        return std::unexpected(buildPool.error());
    }
    auto cmd = buildPool->beginOneShot();
    if (!cmd) {
        return std::unexpected(cmd.error());
    }
    vkCmdBuildAccelerationStructuresKHR(*cmd, 1, &buildInfo, rangeInfos);
    if (const VkResult result = buildPool->endOneShot(*cmd); result != VK_SUCCESS) {
        return std::unexpected(result);
    }

    Micromap out{};
    out.m_dataBuffer = std::move(*dataBuffer);
    out.m_triangleBuffer = std::move(*triangleBuffer);
    out.m_indexBuffer = std::move(*indexBuffer);
    out.m_as = std::move(*micromap);
    out.m_scratch = std::move(*scratch);
    out.m_indexCount = static_cast<std::uint32_t>(indices.size());

    Logger::info("MicromapBuilder '{}': {} base triangles ({} records, {}B data), micromap {}B",
                 base,
                 out.m_indexCount,
                 group.triangles.size(),
                 group.dataBits.size(),
                 sizes.accelerationStructureSize);
    return out;
}

} // namespace harmonia
