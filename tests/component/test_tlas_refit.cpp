// PERF3 slice receipt (Harmonia TlasBuilder refit + AS compaction):
//
// (1) A TLAS built with allowUpdate, when its instance transforms move, is
//     updated in place by refitTlas so traces match a full rebuild of the moved
//     scene — payloads compared BIT-EXACT (same ray, same triangles → the
//     closest-hit distance is traversal-order independent).
// (2) The unmoved instance keeps its distances across the refit.
// (3) compaction (compactAccelerationStructure) preserves traces through the
//     compacted AS, with a size receipt logged.
//
// Trace rig: the PERF4 probe shaders (rays in → payload out: distance +
// primitive id) via an RT pipeline + descriptor buffer, identical to
// test_perf4_reorder_probe.cpp. Local GPU receipts — no CI dependency.

#include <volk/volk.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "fixtures/VulkanTestFixture.hpp"
#include "harmonia/core/Buffer.hpp"
#include "harmonia/core/DescriptorBufferWriter.hpp"
#include "harmonia/core/Logger.hpp"
#include "harmonia/core/ShaderModule.hpp"
#include "harmonia/core/VulkanHandle.hpp"
#include "harmonia/rt/AccelerationStructure.hpp"
#include "harmonia/rt/TlasBuilder.hpp"
#include "harmonia/scene_rt/TriangleMesh.hpp"

namespace {

constexpr std::uint32_t kRayCount = 5U;

struct RayRecord {
    float ox;
    float oy;
    float oz;
    float tmax;
    float dx;
    float dy;
    float dz;
    float pad;
};
static_assert(sizeof(RayRecord) == 32U);

struct RayPayload {
    float distance;
    std::uint32_t primitiveId;
};

void rayTraceToCopyBarrier(VkCommandBuffer cmd) {
    const VkMemoryBarrier2 barrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
        .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT,
    };
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext = nullptr,
        .dependencyFlags = 0,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
        .bufferMemoryBarrierCount = 0,
        .pBufferMemoryBarriers = nullptr,
        .imageMemoryBarrierCount = 0,
        .pImageMemoryBarriers = nullptr,
    };
    vkCmdPipelineBarrier2(cmd, &dependency);
}

/// 2×2×2 box centered at the origin: faces at x/y/z = ±1 (12 triangles).
harmonia::scene::MeshData boxMesh() {
    harmonia::scene::MeshData data;
    const auto addQuad = [&data](sm::float3 a, sm::float3 b, sm::float3 c, sm::float3 d, sm::float3 n) {
        const auto base = static_cast<std::uint32_t>(data.vertices.size());
        for (auto* p : {&a, &b, &c, &d}) {
            harmonia::GpuVertex vertex{};
            vertex.position = *p;
            vertex.normal = n;
            data.vertices.push_back(vertex);
        }
        data.indices.insert(data.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    };
    const sm::float3 up{0, 1, 0};
    const sm::float3 nx{-1, 0, 0};
    const sm::float3 px{1, 0, 0};
    const sm::float3 nz{0, 0, -1};
    const sm::float3 pz{0, 0, 1};
    addQuad({-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}, nx); // -X
    addQuad({1, -1, 1}, {1, -1, -1}, {1, 1, -1}, {1, 1, 1}, px);     // +X
    addQuad({-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1}, {0, -1, 0});
    addQuad({-1, 1, -1}, {-1, 1, 1}, {1, 1, 1}, {1, 1, -1}, up); // +Y
    addQuad({1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}, nz);
    addQuad({-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}, pz);
    return data;
}

/// Two instances of the box: A (custom index 0) at translation x = `ax`,
/// B (custom index 1) fixed at x = 12. Same BLAS referenced twice — the
/// hit DISTANCE against the shared geometry is what discriminates the refit.
[[nodiscard]] std::vector<VkAccelerationStructureInstanceKHR>
makeInstances(const harmonia::scene_rt::TriangleMesh& mesh, float ax) {
    std::vector<VkAccelerationStructureInstanceKHR> instances;
    harmonia::scene::Transform tA{};
    tA.translation = sm::float3(ax, 0.0F, 0.0F);
    harmonia::scene::Transform tB{};
    tB.translation = sm::float3(12.0F, 0.0F, 0.0F);
    instances.push_back(mesh.makeInstance(0U, tA));
    instances.push_back(mesh.makeInstance(1U, tB));
    return instances;
}

/// RT pipeline over the PERF4 probe shaders + descriptor buffer + SBT
/// (one raygen / one miss / one hit record; the probe's rig pattern).
struct RefitRig {
    harmonia::core::UniquePipelineLayout layout;
    harmonia::core::UniquePipeline pipeline;
    harmonia::core::DescriptorBufferWriter writer;
    harmonia::core::Buffer sbt;
    std::uint32_t raygenSize = 0;
    std::uint32_t missSize = 0;
    std::uint32_t hitSize = 0;
    VkStridedDeviceAddressRegionKHR raygenRegion{};
    VkStridedDeviceAddressRegionKHR missRegion{};
    VkStridedDeviceAddressRegionKHR hitRegion{};
    VkStridedDeviceAddressRegionKHR callableRegion{};
};

[[nodiscard]] std::expected<RefitRig, std::string>
createRig(const harmonia::DeviceContext& ctx, std::uint32_t handleSize, const std::filesystem::path& shaderDir) {
    const std::array<VkDescriptorSetLayoutBinding, 3> bindings{{
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr},
        {1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr},
    }};
    const VkDescriptorSetLayoutCreateInfo setLayoutInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
        .bindingCount = static_cast<std::uint32_t>(bindings.size()),
        .pBindings = bindings.data(),
    };
    VkDescriptorSetLayout setLayoutRaw{};
    if (vkCreateDescriptorSetLayout(ctx.device, &setLayoutInfo, nullptr, &setLayoutRaw) != VK_SUCCESS) {
        return std::unexpected("set layout");
    }
    const harmonia::core::UniqueDescriptorSetLayout setLayout{ctx.device, setLayoutRaw};

    const VkPipelineLayoutCreateInfo layoutInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = setLayoutRaw ? &setLayoutRaw : nullptr,
        .pushConstantRangeCount = 0,
        .pPushConstantRanges = nullptr,
    };
    VkPipelineLayout layoutRaw{};
    if (vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &layoutRaw) != VK_SUCCESS) {
        return std::unexpected("pipeline layout");
    }

    auto raygenModule = harmonia::core::createShaderModule(ctx.device, shaderDir / "perf4_probe.rgen.spv");
    auto chitModule = harmonia::core::createShaderModule(ctx.device, shaderDir / "perf4_probe.rchit.spv");
    auto missModule = harmonia::core::createShaderModule(ctx.device, shaderDir / "perf4_probe.rmiss.spv");
    if (!raygenModule || !chitModule || !missModule) {
        return std::unexpected("shader module");
    }
    const std::array<VkPipelineShaderStageCreateInfo, 3> stages{{
        VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR,
            .module = *raygenModule,
            .pName = "main",
            .pSpecializationInfo = nullptr,
        },
        VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
            .module = *chitModule,
            .pName = "main",
            .pSpecializationInfo = nullptr,
        },
        VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage = VK_SHADER_STAGE_MISS_BIT_KHR,
            .module = *missModule,
            .pName = "main",
            .pSpecializationInfo = nullptr,
        },
    }};
    const std::array<VkRayTracingShaderGroupCreateInfoKHR, 3> groups{{
        VkRayTracingShaderGroupCreateInfoKHR{
            .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
            .pNext = nullptr,
            .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
            .generalShader = 0,
            .closestHitShader = VK_SHADER_UNUSED_KHR,
            .anyHitShader = VK_SHADER_UNUSED_KHR,
            .intersectionShader = VK_SHADER_UNUSED_KHR,
            .pShaderGroupCaptureReplayHandle = nullptr,
        },
        VkRayTracingShaderGroupCreateInfoKHR{
            .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
            .pNext = nullptr,
            .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
            .generalShader = VK_SHADER_UNUSED_KHR,
            .closestHitShader = 1,
            .anyHitShader = VK_SHADER_UNUSED_KHR,
            .intersectionShader = VK_SHADER_UNUSED_KHR,
            .pShaderGroupCaptureReplayHandle = nullptr,
        },
        VkRayTracingShaderGroupCreateInfoKHR{
            .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
            .pNext = nullptr,
            .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
            .generalShader = 2,
            .closestHitShader = VK_SHADER_UNUSED_KHR,
            .anyHitShader = VK_SHADER_UNUSED_KHR,
            .intersectionShader = VK_SHADER_UNUSED_KHR,
            .pShaderGroupCaptureReplayHandle = nullptr,
        },
    }};
    const VkRayTracingPipelineCreateInfoKHR pipelineInfo{
        .sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
        .pNext = nullptr,
        .flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
        .stageCount = static_cast<std::uint32_t>(stages.size()),
        .pStages = stages.data(),
        .groupCount = static_cast<std::uint32_t>(groups.size()),
        .pGroups = groups.data(),
        .maxPipelineRayRecursionDepth = 1,
        .pLibraryInfo = nullptr,
        .pLibraryInterface = nullptr,
        .pDynamicState = nullptr,
        .layout = layoutRaw,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = 0,
    };
    VkPipeline pipelineRaw{};
    if (vkCreateRayTracingPipelinesKHR(
            ctx.device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipelineRaw) != VK_SUCCESS) {
        return std::unexpected("rt pipeline");
    }
    vkDestroyShaderModule(ctx.device, *raygenModule, nullptr);
    vkDestroyShaderModule(ctx.device, *chitModule, nullptr);
    vkDestroyShaderModule(ctx.device, *missModule, nullptr);

    RefitRig rig;
    rig.layout = harmonia::core::UniquePipelineLayout{ctx.device, layoutRaw};
    rig.pipeline = harmonia::core::UniquePipeline{ctx.device, pipelineRaw};
    if (!rig.writer.init(ctx, setLayoutRaw, static_cast<std::uint32_t>(bindings.size()), "test.tlasRefit")) {
        return std::unexpected("descriptor buffer");
    }

    // SBT: raygen | miss | hit (one record each; 64-byte alignment).
    const std::uint32_t aligned = (handleSize + 63U) / 64U * 64U;
    rig.raygenSize = aligned;
    rig.missSize = aligned;
    rig.hitSize = aligned * 2U; // closest-hit + any-hit slots (any-hit unused)
    const VkDeviceSize raygenOffset = 0U;
    const VkDeviceSize missOffset = rig.raygenSize;
    const VkDeviceSize hitOffset = missOffset + rig.missSize;
    const VkDeviceSize sbtBytes = static_cast<VkDeviceSize>(hitOffset + rig.hitSize);
    auto sbt = harmonia::core::Buffer::create(ctx,
                                              sbtBytes,
                                              VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
                                                  VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                              VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                              "test.tlasRefit.sbt");
    if (!sbt.has_value() || (*sbt).mappedData() == nullptr) {
        return std::unexpected("sbt buffer");
    }
    rig.sbt = std::move(*sbt);
    std::array<std::uint8_t, 3U * 2U * 64U> handles{};
    if (vkGetRayTracingShaderGroupHandlesKHR(ctx.device, rig.pipeline, 0, 3, handles.size(), handles.data()) !=
        VK_SUCCESS) {
        return std::unexpected("shader group handles");
    }
    auto* sbtBytesPtr = static_cast<std::uint8_t*>(rig.sbt.mappedData());
    std::memcpy(sbtBytesPtr + raygenOffset, handles.data(), handleSize);
    std::memcpy(sbtBytesPtr + missOffset, handles.data() + 2U * handleSize, handleSize);
    std::memcpy(sbtBytesPtr + hitOffset, handles.data() + handleSize, handleSize);
    const VkDeviceAddress sbtAddress = rig.sbt.deviceAddress();
    rig.raygenRegion = {.deviceAddress = sbtAddress + raygenOffset, .stride = rig.raygenSize, .size = rig.raygenSize};
    rig.missRegion = {.deviceAddress = sbtAddress + missOffset, .stride = rig.missSize, .size = rig.missSize};
    rig.hitRegion = {.deviceAddress = sbtAddress + hitOffset, .stride = rig.hitSize, .size = rig.hitSize};
    rig.callableRegion = {};
    return rig;
}

} // namespace

TEST_F(RtFixture, TlasRefit_RefitMatchesFullRebuildAndCompaction) {
    auto& ctx = deviceCtx();

    auto mesh = harmonia::scene_rt::TriangleMesh::create(ctx, commandPool(), boxMesh(), {}, "test.tlasRefit.mesh");
    ASSERT_TRUE(mesh.has_value());
    ASSERT_EQ((*mesh)->buildBlas(ctx, commandPool()), VK_SUCCESS);

    const harmonia::rt::TlasBuildOptions options{.allowUpdate = true, .allowCompaction = true};

    // Phase 1 — build (allowUpdate) + pre-move trace: A at x = 0.
    auto instancesPre = makeInstances(**mesh, 0.0F);
    harmonia::rt::AccelerationStructure tlas;
    VkDeviceAddress tlasAddress{};
    ASSERT_EQ(harmonia::rt::buildTlas(ctx, commandPool(), instancesPre, options, tlas, tlasAddress), VK_SUCCESS);

    const std::filesystem::path shaderDir = HARMONIA_SHADER_DIR;
    const std::uint32_t handleSize = static_cast<std::uint32_t>(physInfo().rtProps.shaderGroupHandleSize);
    auto rig = createRig(ctx, handleSize, shaderDir);
    ASSERT_TRUE(rig.has_value()) << rig.error();

    // Rays (fixed origins/directions/tmax; only instance A moves between phases):
    //   R1 (-5,0,0) +X → pre: A -X face, d ≈ 6 · R2 (5,0,0) +X → pre: B -X face, d ≈ 6
    //   R3 (16,0,0) -X → B +X face, d ≈ 3 (invariant) · R4 (0,3,0) -Y → A top, d ≈ 2
    //   R5 (0,50,0) +Y → miss (invariant)
    std::vector<RayRecord> rays{
        {-5.0F, 0.0F, 0.0F, 50.0F, 1.0F, 0.0F, 0.0F, 0.0F},
        {5.0F, 0.0F, 0.0F, 50.0F, 1.0F, 0.0F, 0.0F, 0.0F},
        {16.0F, 0.0F, 0.0F, 50.0F, -1.0F, 0.0F, 0.0F, 0.0F},
        {0.0F, 3.0F, 0.0F, 50.0F, 0.0F, -1.0F, 0.0F, 0.0F},
        {0.0F, 50.0F, 0.0F, 50.0F, 0.0F, 1.0F, 0.0F, 0.0F},
    };
    auto raysInput = harmonia::core::Buffer::upload(
        ctx,
        commandPool(),
        std::span(reinterpret_cast<const std::byte*>(rays.data()), rays.size() * sizeof(RayRecord)),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        "test.tlasRefit.rays");
    ASSERT_TRUE(raysInput.has_value());
    auto payloads =
        harmonia::core::Buffer::create(ctx,
                                       static_cast<VkDeviceSize>(kRayCount) * sizeof(RayPayload),
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                       VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                       "test.tlasRefit.payloads");
    auto readback = harmonia::core::Buffer::create(ctx,
                                                   static_cast<VkDeviceSize>(kRayCount) * sizeof(RayPayload),
                                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                   VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                                   "test.tlasRefit.readback");
    ASSERT_TRUE(payloads.has_value() && readback.has_value());

    auto trace = [&](VkAccelerationStructureKHR as) -> std::expected<std::vector<RayPayload>, std::string> {
        auto cmd = commandPool().beginOneShot();
        if (!cmd) {
            return std::unexpected("command buffer");
        }
        rig->writer.writeStorageBufferHandle(ctx, 0, *raysInput);
        rig->writer.writeAccelerationStructure(ctx, 1, as);
        rig->writer.writeStorageBufferHandle(ctx, 2, *payloads);
        rig->writer.bind(*cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rig->layout, 0);
        vkCmdBindPipeline(*cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rig->pipeline);
        vkCmdTraceRaysKHR(
            *cmd, &rig->raygenRegion, &rig->missRegion, &rig->hitRegion, &rig->callableRegion, kRayCount, 1, 1);
        rayTraceToCopyBarrier(*cmd);
        const VkBufferCopy copy{0, 0, static_cast<VkDeviceSize>(kRayCount) * sizeof(RayPayload)};
        vkCmdCopyBuffer(*cmd, payloads->handle(), readback->handle(), 1, &copy);
        if (const VkResult result = commandPool().endOneShot(*cmd); result != VK_SUCCESS) {
            return std::unexpected(std::string("submit: VkResult ") + std::to_string(static_cast<int>(result)));
        }
        std::vector<RayPayload> out(kRayCount);
        if (const VkResult readResult = readback->readData(out.data(), out.size() * sizeof(RayPayload));
            readResult != VK_SUCCESS) {
            return std::unexpected(std::string("readback: VkResult ") + std::to_string(static_cast<int>(readResult)));
        }
        return out;
    };

    // Phase 1 — pre-move traces through the refit-built TLAS.
    const auto pre = trace(tlas.handle());
    ASSERT_TRUE(pre.has_value()) << pre.error();
    for (std::uint32_t i = 0; i < kRayCount; ++i) {
        harmonia::core::Logger::info("PERF3 pre[{}]: d={} id={}", i, (*pre)[i].distance, (*pre)[i].primitiveId);
    }
    EXPECT_NEAR((*pre)[0].distance, 4.0F, 0.01F);   // R1: A -X face at x = -1 (origin -5)
    EXPECT_NEAR((*pre)[1].distance, 5.999F, 0.01F); // R2: B -X face at x = 11
    EXPECT_NEAR((*pre)[2].distance, 2.999F, 0.01F); // R3: B +X face at x = 13
    EXPECT_NEAR((*pre)[3].distance, 1.999F, 0.01F); // R4: A top face at y = 1
    EXPECT_EQ((*pre)[4].distance, -1.0F);           // R5: +Y ray misses everything

    // Phase 2 — move A to x = 8 and REFIT in place (instance count/layout
    // unchanged, only the transform differs — the refit contract).
    auto instancesMoved = makeInstances(**mesh, 8.0F);
    ASSERT_EQ(harmonia::rt::refitTlas(ctx, commandPool(), instancesMoved, options, tlas), VK_SUCCESS);
    const auto post = trace(tlas.handle());
    ASSERT_TRUE(post.has_value()) << post.error();
    for (std::uint32_t i = 0; i < kRayCount; ++i) {
        harmonia::core::Logger::info("PERF3 post[{}]: d={} id={}", i, (*post)[i].distance, (*post)[i].primitiveId);
    }
    // R1: A's -X face moved from x = -1 to x = 7 → 5.999 → 11.999.
    EXPECT_NEAR((*post)[0].distance, 11.999F, 0.01F);
    // R2: A's -X face now occludes B → first hit 1.999 (was 5.999).
    EXPECT_NEAR((*post)[1].distance, 1.999F, 0.01F);
    // R3: B is unmoved → invariant.
    EXPECT_NEAR((*post)[2].distance, 3.0F, 0.01F);
    // R4: A moved to x = 8, the (0, y) column is empty → miss.
    EXPECT_EQ((*post)[3].distance, -1.0F);
    // R5: still a miss.
    EXPECT_EQ((*post)[4].distance, -1.0F);

    // Phase 3 — full rebuild of the moved scene: traces must match the refit
    // TLAS BIT-EXACT (same ray, same triangles → same closest hit; traversal
    // order does not change the closest-hit distance or the hit instance).
    harmonia::rt::AccelerationStructure tlasRebuilt;
    VkDeviceAddress tlasRebuiltAddress{};
    ASSERT_EQ(harmonia::rt::buildTlas(ctx, commandPool(), instancesMoved, options, tlasRebuilt, tlasRebuiltAddress),
              VK_SUCCESS);
    const auto rebuilt = trace(tlasRebuilt.handle());
    ASSERT_TRUE(rebuilt.has_value()) << rebuilt.error();
    for (std::uint32_t i = 0; i < kRayCount; ++i) {
        harmonia::core::Logger::info(
            "PERF3 rebuilt[{}]: d={} id={}", i, (*rebuilt)[i].distance, (*rebuilt)[i].primitiveId);
    }
    for (std::uint32_t i = 0; i < kRayCount; ++i) {
        EXPECT_EQ(std::memcmp(&(*post)[i].distance, &(*rebuilt)[i].distance, sizeof(float)), 0)
            << "ray " << i << ": refit vs rebuilt distance mismatch";
        EXPECT_EQ((*post)[i].primitiveId, (*rebuilt)[i].primitiveId) << "ray " << i << ": primitive id mismatch";
    }

    // Phase 4 — compaction: compact the TLAS, rebind, retrace — the compacted
    // AS must trace identically. Sizes logged as the receipt.
    auto compactAS = harmonia::rt::compactAccelerationStructure(ctx, tlas, "test.tlasRefit.compact");
    ASSERT_TRUE(compactAS.has_value()) << compactAS.error();
    harmonia::core::Logger::info("PERF3 probe: TLAS compaction {} -> {} bytes",
                                 static_cast<long long>(tlas.buffer().size()),
                                 static_cast<long long>(compactAS->buffer().size()));
    const auto compacted = trace(compactAS->handle());
    ASSERT_TRUE(compacted.has_value()) << compacted.error();
    // Compaction is a re-encode: hit DISTANCES may differ in low bits (the
    // driver may re-encode instance/precision data), so the receipt is the
    // same hits — primitive ids bit-equal, distances within tolerance.
    for (std::uint32_t i = 0; i < kRayCount; ++i) {
        EXPECT_EQ((*post)[i].primitiveId, (*compacted)[i].primitiveId)
            << "ray " << i << ": compacted vs refit primitive id mismatch";
        EXPECT_NEAR((*post)[i].distance, (*compacted)[i].distance, 0.01F)
            << "ray " << i << ": compacted vs refit distance mismatch";
    }
}
