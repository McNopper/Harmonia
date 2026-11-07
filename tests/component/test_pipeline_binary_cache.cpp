// VK5 adoption test (VK_KHR_pipeline_binary cache): capture → persist →
// fresh-instance reload → consume, plus the global-key invalidation contract
// (driver/feature/layer change ⇒ stored key mismatch ⇒ prune ⇒ re-capture).
//
// Exercises the SAME path the production sites use — the static
// PipelineBinaryCache::computePipeline dispatcher through the DeviceContext
// hook — with a throwaway cache directory. Dispatch verifies each pipeline's
// output (input + 1), proving consume-built pipelines work, not just exist.
// Receipts only.

#include <volk/volk.h>

#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "fixtures/VulkanTestFixture.hpp"
#include "harmonia/core/Buffer.hpp"
#include "harmonia/core/DescriptorBufferWriter.hpp"
#include "harmonia/core/Logger.hpp"
#include "harmonia/core/PipelineBinaryCache.hpp"
#include "harmonia/core/ShaderModule.hpp"
#include "harmonia/core/VulkanHandle.hpp"

namespace {

constexpr std::uint32_t kElementCount = 128U;
constexpr std::uint32_t kWorkgroupSize = 64U;

void computeBarrier(VkCommandBuffer cmd) {
    const VkMemoryBarrier2 barrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT |
                         VK_ACCESS_2_TRANSFER_WRITE_BIT,
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

/// Layout + descriptor buffer; the set layout is held for the rig's lifetime
/// (descriptor-buffer pipeline layouts reference their set layouts).
struct Rig {
    harmonia::core::UniqueDescriptorSetLayout setLayout;
    harmonia::core::UniquePipelineLayout layout;
    harmonia::core::DescriptorBufferWriter writer;

    [[nodiscard]] static std::expected<Rig, std::string> create(const harmonia::DeviceContext& ctx) {
        const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, // input
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, // output
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
        Rig rig;
        rig.setLayout = harmonia::core::UniqueDescriptorSetLayout{ctx.device, setLayoutRaw};
        rig.layout = harmonia::core::UniquePipelineLayout{ctx.device, layoutRaw};

        if (!rig.writer.init(
                ctx, setLayoutRaw, static_cast<std::uint32_t>(bindings.size()), "test.pipelineBinaryCache")) {
            return std::unexpected("descriptor buffer");
        }
        return rig;
    }
};

} // namespace

TEST_F(VulkanFixture, PipelineBinaryCache_CapturePersistConsume) {
    // Non-const context: the test attaches its throwaway cache instance to the
    // DeviceContext exactly like the App does (and detaches it before teardown).
    ASSERT_NE(g_vulkanTestCtx->context, nullptr);
    auto& ctx = g_vulkanTestCtx->context->deviceContext();
    if (!ctx.pipelineBinarySupported || vkGetPipelineKeyKHR == nullptr) {
        GTEST_SKIP() << "VK_KHR_pipeline_binary not available";
    }

    const std::filesystem::path cacheDir = std::filesystem::temp_directory_path() / "harmonia.pipelineBinaryCache.test";
    std::error_code fsError;
    std::filesystem::remove_all(cacheDir, fsError);
    fsError.clear();

    std::vector<std::uint32_t> input(kElementCount);
    for (std::uint32_t i = 0; i < kElementCount; ++i) {
        input[i] = i * 17U + 3U;
    }
    auto inputHost =
        harmonia::core::Buffer::create(ctx,
                                       static_cast<VkDeviceSize>(kElementCount) * sizeof(std::uint32_t),
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                       VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                       "test.pipelineBinaryCache.inputHost");
    ASSERT_TRUE(inputHost.has_value());
    std::memcpy(inputHost->mappedData(), input.data(), inputHost->size());
    auto output =
        harmonia::core::Buffer::create(ctx,
                                       inputHost->size(),
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                       VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                       "test.pipelineBinaryCache.output");
    auto outputReadback = harmonia::core::Buffer::create(ctx,
                                                         inputHost->size(),
                                                         VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                         VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                                         "test.pipelineBinaryCache.readback");
    ASSERT_TRUE(output.has_value() && outputReadback.has_value());

    auto rig = Rig::create(ctx);
    ASSERT_TRUE(rig.has_value()) << rig.error();
    rig->writer.writeStorageBufferHandle(ctx, 0, *inputHost);
    rig->writer.writeStorageBufferHandle(ctx, 1, *output);

    const std::filesystem::path spv = std::filesystem::path(HARMONIA_SHADER_DIR) / "pipeline_binary_probe.comp.spv";

    auto entryCount = [&](void) {
        std::size_t entries = 0;
        std::error_code itError;
        for (const auto& item : std::filesystem::directory_iterator(cacheDir, itError)) {
            if (item.is_regular_file() && item.path().extension() == ".pbinc") {
                ++entries;
            }
        }
        return entries;
    };

    auto dispatchAndVerify = [&](VkPipeline pipeline) {
        auto cmd = commandPool().beginOneShot();
        ASSERT_TRUE(cmd.has_value());
        rig->writer.bind(*cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rig->layout, 0);
        vkCmdBindPipeline(*cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdDispatch(*cmd, kElementCount / kWorkgroupSize, 1, 1);
        computeBarrier(*cmd);
        const VkBufferCopy outputCopy{0, 0, output->size()};
        vkCmdCopyBuffer(*cmd, output->handle(), outputReadback->handle(), 1, &outputCopy);
        ASSERT_EQ(commandPool().endOneShot(*cmd), VK_SUCCESS);
        const auto* results = static_cast<const std::uint32_t*>(outputReadback->mappedData());
        for (std::uint32_t i = 0; i < kElementCount; ++i) {
            ASSERT_EQ(results[i], input[i] + 1U) << "wrong output at " << i;
        }
    };

    auto makePipeline = [&](const char* name) -> std::expected<VkPipeline, std::string> {
        auto module = harmonia::core::createShaderModule(ctx.device, spv);
        if (!module) {
            return std::unexpected("shader module: " + spv.string());
        }
        const VkComputePipelineCreateInfo info{
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
            .layout = rig->layout,
            .basePipelineHandle = VK_NULL_HANDLE,
            .basePipelineIndex = 0,
        };
        auto pipeline = harmonia::core::PipelineBinaryCache::computePipeline(ctx, info, name);
        vkDestroyShaderModule(ctx.device, *module, nullptr);
        if (!pipeline) {
            return std::unexpected("pipeline creation failed: VkResult " +
                                   std::to_string(static_cast<int>(pipeline.error())));
        }
        return *pipeline;
    };

    // Phase 1 — capture: first run misses (compiles), stores the entry to disk.
    {
        auto cache = harmonia::core::PipelineBinaryCache::create(ctx, cacheDir);
        ASSERT_TRUE(cache.has_value()) << cache.error();
        ctx.pipelineBinaryCache = &*cache;
        auto pipeline = makePipeline("cache.test.phase1");
        ASSERT_TRUE(pipeline.has_value()) << pipeline.error();
        EXPECT_EQ(cache->receipts().misses, 1U);
        EXPECT_EQ(cache->receipts().hits, 0U);
        EXPECT_EQ(entryCount(), 1U);
        dispatchAndVerify(*pipeline);
        vkDestroyPipeline(ctx.device, *pipeline, nullptr);
        ctx.pipelineBinaryCache = nullptr;
    }

    // Phase 2 — reload + consume: a fresh instance over the same directory must
    // hit (no prune — the global key still matches) and produce a working
    // pipeline from the stored blobs.
    {
        auto cache = harmonia::core::PipelineBinaryCache::create(ctx, cacheDir);
        ASSERT_TRUE(cache.has_value()) << cache.error();
        EXPECT_EQ(cache->receipts().pruned, 0U);
        ctx.pipelineBinaryCache = &*cache;
        auto pipeline = makePipeline("cache.test.phase2");
        ASSERT_TRUE(pipeline.has_value()) << pipeline.error();
        EXPECT_EQ(cache->receipts().hits, 1U);
        EXPECT_EQ(cache->receipts().misses, 0U);
        dispatchAndVerify(*pipeline);
        vkDestroyPipeline(ctx.device, *pipeline, nullptr);
        ctx.pipelineBinaryCache = nullptr;
    }

    // Phase 3 — global-key invalidation: corrupt the stored key (the state a
    // driver update produces) → the next open must prune the stale entry and
    // re-capture.
    {
        std::ofstream garbage(cacheDir / "globalkey", std::ios::binary | std::ios::trunc);
        garbage << '\x01' << '\x02' << '\x03';
        garbage.close();
        auto cache = harmonia::core::PipelineBinaryCache::create(ctx, cacheDir);
        ASSERT_TRUE(cache.has_value()) << cache.error();
        EXPECT_GE(cache->receipts().pruned, 1U);
        ctx.pipelineBinaryCache = &*cache;
        auto pipeline = makePipeline("cache.test.phase3");
        ASSERT_TRUE(pipeline.has_value()) << pipeline.error();
        EXPECT_EQ(cache->receipts().misses, 1U);
        EXPECT_EQ(entryCount(), 1U);
        dispatchAndVerify(*pipeline);
        vkDestroyPipeline(ctx.device, *pipeline, nullptr);
        ctx.pipelineBinaryCache = nullptr;
    }

    std::filesystem::remove_all(cacheDir, fsError);
}
