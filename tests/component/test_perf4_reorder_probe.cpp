// PERF4 feasibility spike (Meister, Boksansky, Guthe, Bittner 2025,
// arXiv:2506.11273): does ray reordering pay on the dev GPU's RT cores?
//
// 1M secondary-style rays (plane-scattered origins, hemisphere-dominant random
// directions) traced through a minimal RT pipeline against a procedural AS:
//   arm 0: unsorted (baseline)
//   arm 1: sorted by the 2020 direction key (octant + quantized sphere coords)
//   arm 2: sorted by the 2025 termination-point key (two quantized points along
//          the ray; constant termination-distance estimate)
// CPU sort cost is measured as the upper-bound proxy for a GPU radix sort
// (literature: ~1 ms per million rays). Timings are receipts, never a gate.
//
// Validity contract: the sorted arms trace the SAME rays — outputs reordered by
// the sort permutation must equal the baseline payloads EXACTLY.

#include <volk/volk.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <gtest/gtest.h>
#include <random>
#include <span>
#include <vector>

#include "fixtures/VulkanTestFixture.hpp"
#include "harmonia/core/Buffer.hpp"
#include "harmonia/core/DescriptorBufferWriter.hpp"
#include "harmonia/core/Logger.hpp"
#include "harmonia/core/ShaderModule.hpp"
#include "harmonia/core/VulkanHandle.hpp"
#include "harmonia/rt/TlasBuilder.hpp"
#include "harmonia/scene_rt/TriangleMesh.hpp"

namespace {

constexpr std::uint32_t kRayCount = 1U << 20U; // 1 048 576 rays = 32 MiB of records
constexpr std::uint32_t kIterations = 32U;
constexpr std::uint32_t kTracesPerIteration = 4U;
constexpr float kTerminationEstimate = 60.0F; // scene-scale constant distance estimate

/// ray-tracing-shader writes -> transfer/compute reads (payload copy safety).
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

struct RayRecord {
    float ox, oy, oz, tmax;
    float dx, dy, dz, pad;
};
static_assert(sizeof(RayRecord) == 32U);

struct ProbePayload {
    float distance;
    std::uint32_t primitiveId;
};

std::vector<RayRecord> makeRays() {
    std::mt19937_64 rng{20261106ULL};
    std::uniform_real_distribution<float> unit(0.0F, 1.0F);
    std::vector<RayRecord> rays(kRayCount);
    for (std::uint32_t i = 0; i < kRayCount; ++i) {
        RayRecord& r = rays[i];
        r.ox = unit(rng) * 22.0F - 11.0F;
        r.oy = unit(rng) * 5.0F + 1.0F;
        r.oz = unit(rng) * 22.0F - 11.0F;
        r.tmax = 100.0F;
        // down-biased directions (hit-heavy, like diffuse secondary rays over a
        // scene), with strong direction spread — the divergence reordering targets.
        float x = (unit(rng) * 2.0F - 1.0F) * 0.9F;
        float y = -(0.25F + unit(rng) * 0.75F);
        float z = (unit(rng) * 2.0F - 1.0F) * 0.9F;
        const float len = std::sqrt(x * x + y * y + z * z);
        r.dx = x / len;
        r.dy = y / len;
        r.dz = z / len;
        r.pad = 0.0F;
    }
    return rays;
}

// Direction key (Meister 2020): octant (3b) + quantized spherical coordinates.
std::uint32_t directionKey(const RayRecord& r) {
    const float len = std::sqrt(r.dx * r.dx + r.dy * r.dy + r.dz * r.dz);
    const float x = r.dx / len;
    const float y = r.dy / len;
    const float z = r.dz / len;
    const std::uint32_t octant = (x < 0.0F ? 1U : 0U) | (y < 0.0F ? 2U : 0U) | (z < 0.0F ? 4U : 0U);
    const auto theta = static_cast<std::uint32_t>(std::acos(std::clamp(y, -1.0F, 1.0F)) / 3.14159265F * 15.0F);
    const auto phi = static_cast<std::uint32_t>((std::atan2(z, x) + 3.14159265F) / 6.28318530F * 63.0F);
    return (octant << 10U) | ((theta & 15U) << 6U) | (phi & 63U);
}

// Termination-point key (Meister 2025): two quantized points along the ray
// (origin + estimated termination point), packed to 30 bits.
std::uint32_t terminationKey(const RayRecord& r) {
    const auto quantize = [](float v) {
        const auto q = static_cast<std::int32_t>((v + 170.0F) / 340.0F * 31.0F);
        return static_cast<std::uint32_t>(std::clamp(q, 0, 31));
    };
    const std::uint32_t m1 = (quantize(r.ox) << 10U) | (quantize(r.oy) << 5U) | quantize(r.oz);
    const float tx = r.ox + r.dx * kTerminationEstimate;
    const float ty = r.oy + r.dy * kTerminationEstimate;
    const float tz = r.oz + r.dz * kTerminationEstimate;
    const std::uint32_t m2 = (quantize(tx) << 10U) | (quantize(ty) << 5U) | quantize(tz);
    return (m1 << 15U) | m2;
}

/// Sort by key; returns the permutation (sorted position -> original index) + CPU sort cost ms.
std::pair<std::vector<std::uint32_t>, double> sortByKey(const std::vector<RayRecord>& rays,
                                                        std::uint32_t (*key)(const RayRecord&)) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> keyed(kRayCount);
    for (std::uint32_t i = 0; i < kRayCount; ++i) {
        keyed[i] = {key(rays[i]), i};
    }
    const auto started = std::chrono::steady_clock::now();
    std::sort(keyed.begin(), keyed.end());
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    std::vector<std::uint32_t> permutation(kRayCount);
    for (std::uint32_t i = 0; i < kRayCount; ++i) {
        permutation[i] = keyed[i].second;
    }
    return {permutation, ms};
}

std::vector<RayRecord> permuteRays(const std::vector<RayRecord>& rays, const std::vector<std::uint32_t>& permutation) {
    std::vector<RayRecord> out(kRayCount);
    for (std::uint32_t i = 0; i < kRayCount; ++i) {
        out[i] = rays[permutation[i]];
    }
    return out;
}

struct ProbeRig {
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

std::expected<ProbeRig, std::string>
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
        .pSetLayouts = &setLayoutRaw,
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

    ProbeRig rig;
    rig.layout = harmonia::core::UniquePipelineLayout{ctx.device, layoutRaw};
    rig.pipeline = harmonia::core::UniquePipeline{ctx.device, pipelineRaw};
    if (!rig.writer.init(ctx, setLayoutRaw, static_cast<std::uint32_t>(bindings.size()), "test.perf4Probe")) {
        return std::unexpected("descriptor buffer");
    }

    // SBT: raygen | miss | hit (one record each; sizes/strides/offsets aligned to
    // shaderGroupBaseAlignment = 64, which the region device addresses require).
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
                                              "test.perf4Probe.sbt");
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

TEST_F(RtFixture, Perf4Probe_SortedTraceYieldOnDevGpu) {
    auto& ctx = deviceCtx();

    // Procedural AS: ground plane + two boxes.
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
    addQuad({-12, 0, -12}, {12, 0, -12}, {12, 0, 12}, {-12, 0, 12}, up);
    // Box field: 6x6 boxes of varying heights - real traversal work per ray.
    for (std::uint32_t gx = 0; gx < 6U; ++gx) {
        for (std::uint32_t gz = 0; gz < 6U; ++gz) {
            const float x0 = -11.0F + static_cast<float>(gx) * 4.0F;
            const float z0 = -11.0F + static_cast<float>(gz) * 4.0F;
            const float h = 1.0F + static_cast<float>(((gx * 7U + gz * 3U) % 5U)) * 0.8F;
            const float x1 = x0 + 2.2F;
            const float z1 = z0 + 2.2F;
            const sm::float3 nx{1, 0, 0};
            const sm::float3 nz{0, 0, 1};
            addQuad({x0, 0, z0}, {x0, 0, z1}, {x0, h, z1}, {x0, h, z0}, {-1, 0, 0});
            addQuad({x1, 0, z0}, {x1, 0, z1}, {x1, h, z1}, {x1, h, z0}, nx);
            addQuad({x0, 0, z0}, {x1, 0, z0}, {x1, h, z0}, {x0, h, z0}, {0, 0, -1});
            addQuad({x0, 0, z1}, {x1, 0, z1}, {x1, h, z1}, {x0, h, z1}, nz);
            addQuad({x0, h, z0}, {x1, h, z0}, {x1, h, z1}, {x0, h, z1}, up);
        }
    }

    auto mesh = harmonia::scene_rt::TriangleMesh::create(ctx, commandPool(), std::move(data), {}, "test.perf4.mesh");
    ASSERT_TRUE(mesh.has_value());
    ASSERT_EQ((*mesh)->buildBlas(ctx, commandPool()), VK_SUCCESS);
    harmonia::rt::AccelerationStructure tlas;
    VkDeviceAddress tlasAddress{};
    // 256 instances of the box-field mesh spread over +-100 — pushes TLAS
    // traversal + cache pressure toward the production regime the paper measures.
    std::vector<decltype((*mesh)->makeInstance(0U, harmonia::scene::Transform{}))> instances;
    for (std::uint32_t ix = 0; ix < 16U; ++ix) {
        for (std::uint32_t iz = 0; iz < 16U; ++iz) {
            for (std::uint32_t iy = 0; iy < 8U; ++iy) {
                harmonia::scene::Transform transform{};
                transform.translation = sm::float3(-160.0F + static_cast<float>(ix) * 21.0F,
                                                   static_cast<float>(iy) * 22.0F,
                                                   -160.0F + static_cast<float>(iz) * 21.0F);
                instances.push_back((*mesh)->makeInstance(instances.size(), transform));
            }
        }
    }
    ASSERT_EQ(harmonia::rt::buildTlas(ctx, commandPool(), instances, tlas, tlasAddress), VK_SUCCESS);

    const std::filesystem::path shaderDir = HARMONIA_SHADER_DIR;
    const std::uint32_t handleSize = static_cast<std::uint32_t>(physInfo().rtProps.shaderGroupHandleSize);
    auto rig = createRig(ctx, handleSize, shaderDir);
    ASSERT_TRUE(rig.has_value()) << rig.error();

    const auto rays = makeRays();
    const auto bindings = [&](const harmonia::core::Buffer& input, harmonia::core::Buffer& payloads) {
        rig->writer.writeStorageBufferHandle(ctx, 0, input);
        rig->writer.writeAccelerationStructure(ctx, 1, tlas.handle());
        rig->writer.writeStorageBufferHandle(ctx, 2, payloads);
    };

    auto makePayloads = [&](const char* name) {
        return harmonia::core::Buffer::create(ctx,
                                              static_cast<VkDeviceSize>(kRayCount) * sizeof(ProbePayload),
                                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                                  VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                              VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                              name);
    };
    auto makeReadback = [&](const char* name) {
        return harmonia::core::Buffer::create(ctx,
                                              static_cast<VkDeviceSize>(kRayCount) * sizeof(ProbePayload),
                                              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                              VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                              name);
    };
    auto payloadsBase = makePayloads("test.perf4.payloadsBase");
    auto readbackBase = makeReadback("test.perf4.readbackBase");
    auto inputBase = harmonia::core::Buffer::upload(
        ctx,
        commandPool(),
        std::span(reinterpret_cast<const std::byte*>(rays.data()), rays.size() * sizeof(RayRecord)),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        "test.perf4.raysBase");
    ASSERT_TRUE(payloadsBase.has_value() && readbackBase.has_value() && inputBase.has_value());
    bindings(*inputBase, *payloadsBase);

    // Baseline (unsorted) trace once for the validity oracle: trace into the
    // device-local payload buffer (what the timing measures), then copy untimed
    // into the host-visible readback (readData needs a persistent mapping).
    {
        auto cmd = commandPool().beginOneShot();
        ASSERT_TRUE(cmd.has_value());
        rig->writer.bind(*cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rig->layout, 0);
        vkCmdBindPipeline(*cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rig->pipeline);
        vkCmdTraceRaysKHR(
            *cmd, &rig->raygenRegion, &rig->missRegion, &rig->hitRegion, &rig->callableRegion, kRayCount, 1, 1);
        rayTraceToCopyBarrier(*cmd);
        const VkBufferCopy copy{0, 0, static_cast<VkDeviceSize>(kRayCount) * sizeof(ProbePayload)};
        vkCmdCopyBuffer(*cmd, payloadsBase->handle(), readbackBase->handle(), 1, &copy);
        ASSERT_EQ(commandPool().endOneShot(*cmd), VK_SUCCESS);
    }
    std::vector<ProbePayload> baseline(kRayCount);
    ASSERT_EQ(readbackBase->readData(baseline.data(), baseline.size() * sizeof(ProbePayload)), VK_SUCCESS);

    // Timed baseline arm: unsorted rays.
    double baselineMs = 0.0;
    {
        const auto started = std::chrono::steady_clock::now();
        for (std::uint32_t iteration = 0; iteration < kIterations; ++iteration) {
            auto cmd = commandPool().beginOneShot();
            ASSERT_TRUE(cmd.has_value());
            rig->writer.bind(*cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rig->layout, 0);
            vkCmdBindPipeline(*cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rig->pipeline);
            for (std::uint32_t t = 0; t < kTracesPerIteration; ++t) {
                vkCmdTraceRaysKHR(
                    *cmd, &rig->raygenRegion, &rig->missRegion, &rig->hitRegion, &rig->callableRegion, kRayCount, 1, 1);
            }
            ASSERT_EQ(commandPool().endOneShot(*cmd), VK_SUCCESS);
        }
        baselineMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() /
                     static_cast<double>(kIterations * kTracesPerIteration);
        harmonia::core::Logger::info("PERF4 probe [unsorted]: trace {:.4f} ms/1M rays", baselineMs);
    }

    struct Arm {
        const char* name;
        std::uint32_t (*key)(const RayRecord&);
    };
    const std::array<Arm, 2> arms{{{"direction-key", directionKey}, {"termination-key", terminationKey}}};

    for (const Arm& arm : arms) {
        const auto [permutation, sortMs] = sortByKey(rays, arm.key);
        const auto ordered = permuteRays(rays, permutation);
        auto inputSorted = harmonia::core::Buffer::upload(
            ctx,
            commandPool(),
            std::span(reinterpret_cast<const std::byte*>(ordered.data()), ordered.size() * sizeof(RayRecord)),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            "test.perf4.raysSorted");
        auto payloadsSorted = makePayloads("test.perf4.payloadsSorted");
        auto readbackSorted = makeReadback("test.perf4.readbackSorted");
        ASSERT_TRUE(inputSorted.has_value() && payloadsSorted.has_value() && readbackSorted.has_value());
        bindings(*inputSorted, *payloadsSorted);

        // Validity: same rays in a different order -> reordered payloads identical.
        {
            auto cmd = commandPool().beginOneShot();
            ASSERT_TRUE(cmd.has_value());
            rig->writer.bind(*cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rig->layout, 0);
            vkCmdBindPipeline(*cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rig->pipeline);
            vkCmdTraceRaysKHR(
                *cmd, &rig->raygenRegion, &rig->missRegion, &rig->hitRegion, &rig->callableRegion, kRayCount, 1, 1);
            rayTraceToCopyBarrier(*cmd);
            const VkBufferCopy copy{0, 0, static_cast<VkDeviceSize>(kRayCount) * sizeof(ProbePayload)};
            vkCmdCopyBuffer(*cmd, payloadsSorted->handle(), readbackSorted->handle(), 1, &copy);
            ASSERT_EQ(commandPool().endOneShot(*cmd), VK_SUCCESS);
        }
        std::vector<ProbePayload> sortedPayloads(kRayCount);
        ASSERT_EQ(readbackSorted->readData(sortedPayloads.data(), sortedPayloads.size() * sizeof(ProbePayload)),
                  VK_SUCCESS);
        for (std::uint32_t i = 0; i < kRayCount; ++i) {
            const auto& expected = baseline[permutation[i]];
            // Distance compared exactly. primitiveId is deliberately NOT compared:
            // rays that hit a shared quad edge resolve to either triangle by
            // implementation-defined traversal tie-breaking, which reordering can
            // flip (the hit DISTANCE is identical either way).
            ASSERT_FLOAT_EQ(sortedPayloads[i].distance, expected.distance) << arm.name << " ray " << i;
        }

        // Timed arm: 32 iterations x 4 back-to-back traces (WAW same values).
        const auto started = std::chrono::steady_clock::now();
        for (std::uint32_t iteration = 0; iteration < kIterations; ++iteration) {
            auto cmd = commandPool().beginOneShot();
            ASSERT_TRUE(cmd.has_value());
            rig->writer.bind(*cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rig->layout, 0);
            vkCmdBindPipeline(*cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rig->pipeline);
            for (std::uint32_t t = 0; t < kTracesPerIteration; ++t) {
                vkCmdTraceRaysKHR(
                    *cmd, &rig->raygenRegion, &rig->missRegion, &rig->hitRegion, &rig->callableRegion, kRayCount, 1, 1);
            }
            ASSERT_EQ(commandPool().endOneShot(*cmd), VK_SUCCESS);
        }
        const double msPerTrace =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() /
            static_cast<double>(kIterations * kTracesPerIteration);

        const double speedup = baselineMs > 0.0 ? baselineMs / msPerTrace : 1.0;
        harmonia::core::Logger::info("PERF4 probe [{}]: trace {:.4f} ms/1M rays (speedup vs unsorted {:.3f}x), "
                                     "CPU sort {:.2f} ms/1M rays",
                                     arm.name,
                                     msPerTrace,
                                     speedup,
                                     sortMs);
        harmonia::core::Logger::info("PERF4 probe net: reordering pays only if (unsorted - sorted) >= sort cost; "
                                     "GPU radix sort literature ~1 ms/1M rays (CPU proxy measured above)");
    }
}
