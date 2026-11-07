// VK5 re-investigation probe (VK_KHR_pipeline_binary, 2026-11-07): the 2026-11-06
// "blocked (driver defect)" verdict was recorded from a spike whose code never
// landed, so its evidence is not reproducible. This probe re-runs both spec paths
// from scratch, following the Khronos proposal + official sample call sequence:
//
//   Path A (capture): pipelines created with VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR
//     (flags2 info — the flags2 chain replaces the legacy 32-bit flags), NULL cache
//     (spec: VkPipelineCache is not allowed alongside binaries) ->
//     vkCreatePipelineBinariesKHR (two-call count/handles) ->
//     vkGetPipelineBinaryDataKHR (two-call size/fetch; the official sample requires a
//     valid VkPipelineBinaryKeyKHR pointer on BOTH calls) ->
//     vkReleaseCapturedPipelineDataKHR. Each pipeline is destroyed right after its
//     acquisition — the recorded failure was order/moving-position dependent, so the
//     sequence deliberately leaves no idle between acquisitions.
//
//   Path B (consume): vkCreatePipelineBinariesKHR(pKeysAndDataInfo) ->
//     VkPipelineBinaryInfoKHR pNext -> pipeline from binary, DISPATCHED with output
//     verified (input + 1) — proving the consume path yields a working pipeline, not
//     just a valid handle. From-scratch creation is timed as the comparison receipt.
//
//   Global key: queried and logged — the cache-validity contract. A driver update
//     changes the global key, which invalidates every cached blob; the pipelines are
//     then re-captured on the first run of the new driver (the app cache prunes
//     stale keys, keyed on this global key).
//
// Receipts only — timings are logged, never gated (shared dev machine).

#include <volk/volk.h>

#include <chrono>
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

namespace {

constexpr std::uint32_t kElementCount = 256U;
constexpr std::uint32_t kWorkgroupSize = 64U;
constexpr std::uint32_t kPipelineCount = 5U; // Theia's cold-start pipeline count

// transfer writes -> compute -> transfer reads (buffer content safety).
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

[[nodiscard]] double sinceMs(const std::chrono::steady_clock::time_point& started) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
}

[[nodiscard]] VkPipelineBinaryKeyKHR makeKey() {
    VkPipelineBinaryKeyKHR key{};
    key.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR;
    key.pNext = nullptr;
    key.keySize = 0;
    return key;
}

[[nodiscard]] bool sameKey(const VkPipelineBinaryKeyKHR& a, const VkPipelineBinaryKeyKHR& b) {
    return a.keySize == b.keySize && (a.keySize == 0 || std::memcmp(a.key, b.key, a.keySize) == 0);
}

struct CapturedBlob {
    VkPipelineBinaryKeyKHR key{};
    std::vector<std::uint8_t> data;
};

/// Layout + descriptor buffer only — the probe creates its pipelines per-arm.
/// The set layout is held for the rig's lifetime: descriptor-buffer pipeline
/// layouts reference their set layouts, which must outlive them (VUID).
struct ProbeRig {
    harmonia::core::UniqueDescriptorSetLayout setLayout;
    harmonia::core::UniquePipelineLayout layout;
    harmonia::core::DescriptorBufferWriter writer;

    [[nodiscard]] static std::expected<ProbeRig, std::string> create(const harmonia::DeviceContext& ctx) {
        const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, // input
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, // output
        }};
        const VkDescriptorSetLayoutCreateInfo setLayoutInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            // MOD1: descriptor-buffer layout (VUID: missing DESCRIPTOR_BUFFER_BIT otherwise).
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
        ProbeRig rig;
        rig.setLayout = harmonia::core::UniqueDescriptorSetLayout{ctx.device, setLayoutRaw};
        rig.layout = harmonia::core::UniquePipelineLayout{ctx.device, layoutRaw};

        if (!rig.writer.init(ctx, setLayoutRaw, static_cast<std::uint32_t>(bindings.size()), "test.pipelineBinary")) {
            return std::unexpected("descriptor buffer");
        }
        return rig;
    }
};

} // namespace

TEST_F(VulkanFixture, PipelineBinary_GlobalKeyAndProperties) {
    auto& ctx = deviceCtx();
    if (!ctx.pipelineBinarySupported || vkGetPipelineKeyKHR == nullptr) {
        GTEST_SKIP() << "VK_KHR_pipeline_binary not available";
    }

    // Global key — the cache-validity contract: when it changes between runs
    // (driver/feature/layer changes), every cached blob is invalid and the
    // pipelines must be re-captured. Same-run stability is the minimum expectation.
    VkPipelineBinaryKeyKHR first = makeKey();
    ASSERT_EQ(vkGetPipelineKeyKHR(ctx.device, nullptr, &first), VK_SUCCESS);
    VkPipelineBinaryKeyKHR second = makeKey();
    ASSERT_EQ(vkGetPipelineKeyKHR(ctx.device, nullptr, &second), VK_SUCCESS);
    ASSERT_GT(first.keySize, 0U);
    EXPECT_EQ(first.keySize, second.keySize);
    EXPECT_TRUE(sameKey(first, second)) << "global key unstable within one run";
    harmonia::core::Logger::info("VK5 probe: global key ({} bytes) {:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
                                 first.keySize,
                                 first.key[0],
                                 first.key[1],
                                 first.key[2],
                                 first.key[3],
                                 first.key[4],
                                 first.key[5],
                                 first.key[6],
                                 first.key[7]);

    VkPhysicalDevicePipelineBinaryPropertiesKHR binaryProperties{};
    binaryProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &binaryProperties;
    vkGetPhysicalDeviceProperties2(ctx.physicalDevice, &properties);
    harmonia::core::Logger::info("VK5 probe: pipelineBinaryPrefersInternalCache={} pipelineBinaryInternalCache={}",
                                 static_cast<std::uint32_t>(binaryProperties.pipelineBinaryPrefersInternalCache),
                                 static_cast<std::uint32_t>(binaryProperties.pipelineBinaryInternalCache));
}

TEST_F(VulkanFixture, PipelineBinary_CaptureConsumeDispatch) {
    auto& ctx = deviceCtx();
    if (!ctx.pipelineBinarySupported || vkCreatePipelineBinariesKHR == nullptr ||
        vkGetPipelineBinaryDataKHR == nullptr || vkReleaseCapturedPipelineDataKHR == nullptr) {
        GTEST_SKIP() << "VK_KHR_pipeline_binary not available";
    }

    std::vector<std::uint32_t> input(kElementCount);
    for (std::uint32_t i = 0; i < kElementCount; ++i) {
        input[i] = i * 13U + 5U;
    }

    auto inputHost =
        harmonia::core::Buffer::create(ctx,
                                       static_cast<VkDeviceSize>(kElementCount) * sizeof(std::uint32_t),
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                       VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                       "test.pipelineBinary.inputHost");
    ASSERT_TRUE(inputHost.has_value());
    std::memcpy(inputHost->mappedData(), input.data(), inputHost->size());

    auto output =
        harmonia::core::Buffer::create(ctx,
                                       inputHost->size(),
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                       VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                       "test.pipelineBinary.output");
    auto outputReadback = harmonia::core::Buffer::create(ctx,
                                                         inputHost->size(),
                                                         VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                         VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                                         "test.pipelineBinary.readback");
    ASSERT_TRUE(output.has_value() && outputReadback.has_value());

    auto rig = ProbeRig::create(ctx);
    ASSERT_TRUE(rig.has_value()) << rig.error();
    rig->writer.writeStorageBufferHandle(ctx, 0, *inputHost);
    rig->writer.writeStorageBufferHandle(ctx, 1, *output);

    const std::filesystem::path spv = std::filesystem::path(HARMONIA_SHADER_DIR) / "pipeline_binary_probe.comp.spv";

    // Capture arm: flags2 carries the bits (the flags2 chain replaces the legacy
    // 32-bit flags per maintenance5); the consume arm below uses the legacy flag.
    auto makePipeline = [&](VkPipelineCreateFlags baseFlags,
                            VkPipelineCreateFlags2KHR flags2Value,
                            const void* extraNext) -> std::expected<VkPipeline, std::string> {
        auto module = harmonia::core::createShaderModule(ctx.device, spv);
        if (!module) {
            return std::unexpected("shader module: " + spv.string());
        }
        VkPipelineCreateFlags2CreateInfoKHR flags2{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR,
            .pNext = nullptr,
            .flags = flags2Value,
        };
        void* chainHead = const_cast<void*>(extraNext);
        if (flags2Value != 0) {
            flags2.pNext = chainHead;
            chainHead = &flags2;
        }
        const VkComputePipelineCreateInfo pipelineInfo{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .pNext = chainHead,
            .flags = baseFlags,
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
        VkPipeline pipelineRaw{};
        const VkResult result =
            vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipelineRaw);
        vkDestroyShaderModule(ctx.device, *module, nullptr);
        if (result != VK_SUCCESS) {
            return std::unexpected("pipeline creation failed: VkResult " + std::to_string(static_cast<int>(result)));
        }
        return pipelineRaw;
    };

    auto acquireBinaries = [&](VkPipeline pipeline) -> std::expected<std::vector<CapturedBlob>, std::string> {
        const VkPipelineBinaryCreateInfoKHR createInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR,
            .pNext = nullptr,
            .pKeysAndDataInfo = nullptr,
            .pipeline = pipeline,
            .pPipelineCreateInfo = nullptr,
        };
        VkPipelineBinaryHandlesInfoKHR handles{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR,
            .pNext = nullptr,
            .pipelineBinaryCount = 0,
            .pPipelineBinaries = nullptr,
        };
        if (vkCreatePipelineBinariesKHR(ctx.device, &createInfo, nullptr, &handles) != VK_SUCCESS) {
            return std::unexpected("vkCreatePipelineBinariesKHR (count query)");
        }
        std::vector<VkPipelineBinaryKHR> binaries(handles.pipelineBinaryCount);
        handles.pPipelineBinaries = binaries.data();
        if (vkCreatePipelineBinariesKHR(ctx.device, &createInfo, nullptr, &handles) != VK_SUCCESS) {
            return std::unexpected("vkCreatePipelineBinariesKHR (handles)");
        }
        std::vector<CapturedBlob> blobs;
        blobs.reserve(handles.pipelineBinaryCount);
        for (const VkPipelineBinaryKHR binary : binaries) {
            const VkPipelineBinaryDataInfoKHR dataInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR,
                .pNext = nullptr,
                .pipelineBinary = binary,
            };
            CapturedBlob blob = {makeKey(), {}};
            size_t dataSize = 0;
            // Two-call pattern; a valid key pointer is required on BOTH calls
            // (official Khronos sample contract).
            if (vkGetPipelineBinaryDataKHR(ctx.device, &dataInfo, &blob.key, &dataSize, nullptr) != VK_SUCCESS) {
                return std::unexpected("vkGetPipelineBinaryDataKHR (size query)");
            }
            blob.data.resize(dataSize);
            if (vkGetPipelineBinaryDataKHR(ctx.device, &dataInfo, &blob.key, &dataSize, blob.data.data()) !=
                VK_SUCCESS) {
                return std::unexpected("vkGetPipelineBinaryDataKHR (fetch)");
            }
            blobs.push_back(std::move(blob));
            vkDestroyPipelineBinaryKHR(ctx.device, binary, nullptr);
        }
        return blobs;
    };

    // Path A: capture sequence. Acquire, release, destroy — no idle between the
    // pipelines (the recorded failure was order/moving-position dependent).
    std::vector<CapturedBlob> pipelineZeroBlobs;
    for (std::uint32_t i = 0; i < kPipelineCount; ++i) {
        const auto createStarted = std::chrono::steady_clock::now();
        auto pipeline = makePipeline(
            0, VK_PIPELINE_CREATE_2_DESCRIPTOR_BUFFER_BIT_EXT | VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR, nullptr);
        ASSERT_TRUE(pipeline.has_value()) << pipeline.error();
        const double createMs = sinceMs(createStarted);

        const auto acquireStarted = std::chrono::steady_clock::now();
        auto acquired = acquireBinaries(*pipeline);
        const double acquireMs = sinceMs(acquireStarted);
        ASSERT_TRUE(acquired.has_value()) << acquired.error();
        ASSERT_FALSE(acquired->empty());
        size_t totalBytes = 0;
        for (const CapturedBlob& blob : *acquired) {
            totalBytes += blob.data.size();
        }
        harmonia::core::Logger::info(
            "VK5 probe: pipeline {} created in {:.3f} ms -> {} binary blob(s), {} bytes, acquired in {:.3f} ms",
            i,
            createMs,
            acquired->size(),
            totalBytes,
            acquireMs);

        if (i == 0) {
            pipelineZeroBlobs = std::move(*acquired);
        } else if (sameKey(pipelineZeroBlobs[0].key, (*acquired)[0].key)) {
            // Expected here: the five create-infos are identical, so the driver may
            // legitimately dedup to one binary (spec: identical keys for identical
            // binaries). Log-only — this is a receipt, not an invariant.
            harmonia::core::Logger::info("VK5 probe: pipeline {} deduped onto pipeline 0's binary key", i);
        }

        const VkReleaseCapturedPipelineDataInfoKHR releaseInfo{
            .sType = VK_STRUCTURE_TYPE_RELEASE_CAPTURED_PIPELINE_DATA_INFO_KHR,
            .pNext = nullptr,
            .pipeline = *pipeline,
        };
        ASSERT_EQ(vkReleaseCapturedPipelineDataKHR(ctx.device, &releaseInfo, nullptr), VK_SUCCESS);
        vkDestroyPipeline(ctx.device, *pipeline, nullptr);
    }
    ASSERT_FALSE(pipelineZeroBlobs.empty());

    // Path B: consume — rebuild the binary handles from the captured blobs, then
    // create a pipeline FROM the binary (NULL cache: spec forbids mixing caches).
    std::vector<VkPipelineBinaryKeyKHR> keys;
    std::vector<VkPipelineBinaryDataKHR> datas;
    keys.reserve(pipelineZeroBlobs.size());
    datas.reserve(pipelineZeroBlobs.size());
    for (CapturedBlob& blob : pipelineZeroBlobs) {
        keys.push_back(blob.key);
        datas.push_back(VkPipelineBinaryDataKHR{.dataSize = blob.data.size(), .pData = blob.data.data()});
    }
    const VkPipelineBinaryKeysAndDataKHR keysAndData{
        .binaryCount = static_cast<std::uint32_t>(keys.size()),
        .pPipelineBinaryKeys = keys.data(),
        .pPipelineBinaryData = datas.data(),
    };
    const VkPipelineBinaryCreateInfoKHR fromDataInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR,
        .pNext = nullptr,
        .pKeysAndDataInfo = &keysAndData,
        .pipeline = VK_NULL_HANDLE,
        .pPipelineCreateInfo = nullptr,
    };
    VkPipelineBinaryHandlesInfoKHR handles{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR,
        .pNext = nullptr,
        .pipelineBinaryCount = 0,
        .pPipelineBinaries = nullptr,
    };
    ASSERT_EQ(vkCreatePipelineBinariesKHR(ctx.device, &fromDataInfo, nullptr, &handles), VK_SUCCESS);
    std::vector<VkPipelineBinaryKHR> fromDataBinaries(handles.pipelineBinaryCount);
    handles.pPipelineBinaries = fromDataBinaries.data();
    ASSERT_EQ(vkCreatePipelineBinariesKHR(ctx.device, &fromDataInfo, nullptr, &handles), VK_SUCCESS);
    const VkPipelineBinaryInfoKHR binaryInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR,
        .pNext = nullptr,
        .binaryCount = handles.pipelineBinaryCount,
        .pPipelineBinaries = fromDataBinaries.data(),
    };

    const auto fromBinaryStarted = std::chrono::steady_clock::now();
    auto fromBinaryPipeline = makePipeline(VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT, 0, &binaryInfo);
    const double fromBinaryMs = sinceMs(fromBinaryStarted);
    ASSERT_TRUE(fromBinaryPipeline.has_value()) << fromBinaryPipeline.error();
    harmonia::core::Logger::info("VK5 probe: pipeline from binary created in {:.3f} ms", fromBinaryMs);

    // From-scratch comparison receipt (the cold-start number VK5 exists to cut).
    const auto fromScratchStarted = std::chrono::steady_clock::now();
    auto fromScratchPipeline = makePipeline(VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT, 0, nullptr);
    const double fromScratchMs = sinceMs(fromScratchStarted);
    ASSERT_TRUE(fromScratchPipeline.has_value()) << fromScratchPipeline.error();
    harmonia::core::Logger::info("VK5 probe: pipeline from scratch created in {:.3f} ms", fromScratchMs);
    vkDestroyPipeline(ctx.device, *fromScratchPipeline, nullptr);

    // Dispatch the BINARY-built pipeline and verify its output (input + 1) — the
    // consume path must produce a working pipeline, not just a valid handle.
    auto cmd = commandPool().beginOneShot();
    ASSERT_TRUE(cmd.has_value());
    rig->writer.bind(*cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rig->layout, 0);
    vkCmdBindPipeline(*cmd, VK_PIPELINE_BIND_POINT_COMPUTE, *fromBinaryPipeline);
    vkCmdDispatch(*cmd, kElementCount / kWorkgroupSize, 1, 1);
    computeBarrier(*cmd);
    const VkBufferCopy outputCopy{0, 0, output->size()};
    vkCmdCopyBuffer(*cmd, output->handle(), outputReadback->handle(), 1, &outputCopy);
    ASSERT_EQ(commandPool().endOneShot(*cmd), VK_SUCCESS);

    const auto* results = static_cast<const std::uint32_t*>(outputReadback->mappedData());
    for (std::uint32_t i = 0; i < kElementCount; ++i) {
        ASSERT_EQ(results[i], input[i] + 1U) << "binary-built pipeline produced wrong output at " << i;
    }

    for (const VkPipelineBinaryKHR binary : fromDataBinaries) {
        vkDestroyPipelineBinaryKHR(ctx.device, binary, nullptr);
    }
    vkDestroyPipeline(ctx.device, *fromBinaryPipeline, nullptr);
}
