// VK4 micro-benchmark: compaction via per-thread atomic (baseline) vs subgroup
// prefix sum (VK4 adoption arm). Both kernels are asserted to produce IDENTICAL
// outputs; timings are logged as receipts and never gated (a shared dev machine
// has no stable performance baseline). Input: ~1/3 non-zero elements.

#include <volk/volk.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>
#include <vector>

#include "fixtures/VulkanTestFixture.hpp"
#include "harmonia/core/Buffer.hpp"
#include "harmonia/core/DescriptorBufferWriter.hpp"
#include "harmonia/core/Logger.hpp"
#include "harmonia/core/ShaderModule.hpp"
#include "harmonia/core/VulkanHandle.hpp"

namespace {
constexpr std::uint32_t kElementCount = 1U << 22U; // 4 Mi elements = 16 MiB input
constexpr std::uint32_t kIterations = 64U;
constexpr std::uint32_t kWorkgroupSize = 256U;

void computeBarrier(VkCommandBuffer cmd) {
    const VkMemoryBarrier2 barrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                         VK_ACCESS_2_TRANSFER_READ_BIT,
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

struct BenchRig {
    harmonia::core::UniquePipelineLayout layout;
    harmonia::core::UniquePipeline pipeline;
    harmonia::core::DescriptorBufferWriter writer;

    [[nodiscard]] static std::expected<BenchRig, std::string> create(const harmonia::DeviceContext& ctx,
                                                                     const std::filesystem::path& spv) {
        const std::array<VkDescriptorSetLayoutBinding, 3> bindings{{
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, // input
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, // output
            {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, // count
        }};
        const VkDescriptorSetLayoutCreateInfo setLayoutInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            // MOD1: descriptor-buffer layout — required for vkGetDescriptorSetLayoutSizeEXT
            // / vkCmdSetDescriptorBufferOffsetsEXT (VUID: missing DESCRIPTOR_BUFFER_BIT).
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
        BenchRig rig;
        rig.layout = harmonia::core::UniquePipelineLayout{ctx.device, layoutRaw};

        auto module = harmonia::core::createShaderModule(ctx.device, spv);
        if (!module) {
            return std::unexpected("shader module: " + spv.string());
        }
        const VkComputePipelineCreateInfo pipelineInfo{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
            .stage =
                VkPipelineShaderStageCreateInfo{
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                    .module = *module,
                    .pName = "main",
                    .pSpecializationInfo = nullptr,
                },
            .layout = rig.layout,
            .basePipelineHandle = VK_NULL_HANDLE,
            .basePipelineIndex = 0,
        };
        VkPipeline pipelineRaw{};
        const VkResult pipelineResult =
            vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipelineRaw);
        vkDestroyShaderModule(ctx.device, *module, nullptr);
        if (pipelineResult != VK_SUCCESS) {
            return std::unexpected("pipeline");
        }
        rig.pipeline = harmonia::core::UniquePipeline{ctx.device, pipelineRaw};

        if (!rig.writer.init(ctx, setLayoutRaw, static_cast<std::uint32_t>(bindings.size()), "test.subgroupBench")) {
            return std::unexpected("descriptor buffer");
        }
        return rig;
    }
};

/// fill count -> dispatch -> copy results into readback; one command buffer per iteration.
double runArm(harmonia::core::CommandPool& pool,
              const BenchRig& rig,
              const harmonia::core::Buffer& output,
              harmonia::core::Buffer& count,
              harmonia::core::Buffer& outputReadback,
              harmonia::core::Buffer& countReadback) {
    const auto started = std::chrono::steady_clock::now();
    for (std::uint32_t iteration = 0; iteration < kIterations; ++iteration) {
        auto cmd = pool.beginOneShot();
        EXPECT_TRUE(cmd.has_value());
        vkCmdFillBuffer(*cmd, count.handle(), 0, sizeof(std::uint32_t), 0);
        computeBarrier(*cmd);
        rig.writer.bind(*cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rig.layout, 0);
        vkCmdBindPipeline(*cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rig.pipeline);
        vkCmdDispatch(*cmd, kElementCount / kWorkgroupSize, 1, 1);
        computeBarrier(*cmd);
        const VkBufferCopy outputCopy{0, 0, output.size()};
        vkCmdCopyBuffer(*cmd, output.handle(), outputReadback.handle(), 1, &outputCopy);
        const VkBufferCopy countCopy{0, 0, sizeof(std::uint32_t)};
        vkCmdCopyBuffer(*cmd, count.handle(), countReadback.handle(), 1, &countCopy);
        EXPECT_EQ(pool.endOneShot(*cmd), VK_SUCCESS);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return ms / static_cast<double>(kIterations);
}
} // namespace

TEST_F(VulkanFixture, SubgroupBench_CompactionArmsAgree) {
    auto& ctx = deviceCtx();

    std::vector<std::uint32_t> input(kElementCount);
    std::uint32_t expectedCount = 0;
    for (std::uint32_t i = 0; i < kElementCount; ++i) {
        input[i] = (i % 3U == 0U) ? (i + 1U) : 0U;
        if (input[i] != 0U) {
            ++expectedCount;
        }
    }

    auto inputHost =
        harmonia::core::Buffer::create(ctx,
                                       static_cast<VkDeviceSize>(kElementCount) * sizeof(std::uint32_t),
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                       VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                       "test.subgroupBench.inputHost");
    ASSERT_TRUE(inputHost.has_value());
    std::memcpy(inputHost->mappedData(), input.data(), inputHost->size());

    auto makeDeviceBuffer = [&](VkDeviceSize bytes, VkBufferUsageFlags usage, const char* name) {
        return harmonia::core::Buffer::create(ctx, bytes, usage, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, name);
    };
    auto outputA = makeDeviceBuffer(inputHost->size(),
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                    "test.subgroupBench.outputA");
    auto outputB = makeDeviceBuffer(inputHost->size(),
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                    "test.subgroupBench.outputB");
    auto countA = makeDeviceBuffer(sizeof(std::uint32_t),
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   "test.subgroupBench.countA");
    auto countB = makeDeviceBuffer(sizeof(std::uint32_t),
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   "test.subgroupBench.countB");
    auto outputReadbackA = harmonia::core::Buffer::create(ctx,
                                                          inputHost->size(),
                                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                          VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                                          "test.subgroupBench.readbackA");
    auto outputReadbackB = harmonia::core::Buffer::create(ctx,
                                                          inputHost->size(),
                                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                          VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                                          "test.subgroupBench.readbackB");
    auto countReadbackA = harmonia::core::Buffer::create(ctx,
                                                         sizeof(std::uint32_t),
                                                         VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                         VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                                         "test.subgroupBench.readbackCountA");
    auto countReadbackB = harmonia::core::Buffer::create(ctx,
                                                         sizeof(std::uint32_t),
                                                         VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                         VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                                         "test.subgroupBench.readbackCountB");
    ASSERT_TRUE(outputA.has_value() && outputB.has_value() && countA.has_value() && countB.has_value());
    ASSERT_TRUE(outputReadbackA.has_value() && outputReadbackB.has_value());
    ASSERT_TRUE(countReadbackA.has_value() && countReadbackB.has_value());

    const std::filesystem::path shaderDir = HARMONIA_SHADER_DIR;
    auto atomicRig = BenchRig::create(ctx, shaderDir / "subgroup_bench_atomic.comp.spv");
    auto scanRig = BenchRig::create(ctx, shaderDir / "subgroup_bench_scan.comp.spv");
    ASSERT_TRUE(atomicRig.has_value()) << atomicRig.error();
    ASSERT_TRUE(scanRig.has_value()) << scanRig.error();

    atomicRig->writer.writeStorageBufferHandle(ctx, 0, *inputHost);
    atomicRig->writer.writeStorageBufferHandle(ctx, 1, *outputA);
    atomicRig->writer.writeStorageBufferHandle(ctx, 2, *countA);
    scanRig->writer.writeStorageBufferHandle(ctx, 0, *inputHost);
    scanRig->writer.writeStorageBufferHandle(ctx, 1, *outputB);
    scanRig->writer.writeStorageBufferHandle(ctx, 2, *countB);

    const double atomicMs = runArm(commandPool(), *atomicRig, *outputA, *countA, *outputReadbackA, *countReadbackA);
    const double scanMs = runArm(commandPool(), *scanRig, *outputB, *countB, *outputReadbackB, *countReadbackB);

    const auto countAValue = *static_cast<const std::uint32_t*>(countReadbackA->mappedData());
    const auto countBValue = *static_cast<const std::uint32_t*>(countReadbackB->mappedData());
    ASSERT_EQ(countAValue, expectedCount);
    ASSERT_EQ(countBValue, expectedCount);

    // Validity contract: BOTH arms must produce the exact multiset of non-zero inputs.
    // Order note (the 10e design insight): the subgroup prefix preserves order WITHIN
    // each workgroup, but the per-workgroup global base reservation is workgroup-
    // scheduling ordered — so neither arm is globally order-preserving. A bit-identical
    // deterministic compaction needs the device-wide hierarchical scan that PERF5 10e's
    // persistent-wavefront structure provides; this bench compares the per-element
    // cost of 1-atomic-per-thread vs 1-atomic-per-subgroup.
    std::vector<std::uint32_t> expectedCompact;
    expectedCompact.reserve(expectedCount);
    for (std::uint32_t i = 0; i < kElementCount; ++i) {
        if (input[i] != 0U) {
            expectedCompact.push_back(input[i]);
        }
    }
    std::sort(expectedCompact.begin(), expectedCompact.end());

    const auto* outA = static_cast<const std::uint32_t*>(outputReadbackA->mappedData());
    const auto* outB = static_cast<const std::uint32_t*>(outputReadbackB->mappedData());
    std::vector<std::uint32_t> atomicCompact(outA, outA + expectedCount);
    std::vector<std::uint32_t> scanCompact(outB, outB + expectedCount);
    std::sort(atomicCompact.begin(), atomicCompact.end());
    std::sort(scanCompact.begin(), scanCompact.end());
    for (std::uint32_t i = 0; i < expectedCount; ++i) {
        ASSERT_EQ(atomicCompact[i], expectedCompact[i]) << "atomic arm (sorted), element " << i;
        ASSERT_EQ(scanCompact[i], expectedCompact[i]) << "scan arm (sorted), element " << i;
    }

    harmonia::core::Logger::info("VK4 compaction bench: {} elements, {} iterations — "
                                 "atomic {:.4f} ms/iter, subgroup-scan {:.4f} ms/iter",
                                 kElementCount,
                                 kIterations,
                                 atomicMs,
                                 scanMs);
}
