// Oracle: a finite per-slot state machine, not a second implementation of SMS.
#include <volk/volk.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <utility>
#include <vector>

#include "fixtures/VulkanTestFixture.hpp"
#include "harmonia/core/Buffer.hpp"
#include "harmonia/core/VulkanHandle.hpp"
#include "harmonia/pipeline/SmsPass.hpp"

namespace {
using Pass = harmonia::pipeline::SmsPass;
using Completion = harmonia::pipeline::SmsQueueCompletion;
using Record = std::array<std::uint32_t, 64>;
static_assert(sizeof(Record) == Pass::kWorkRecordSize);

class SmsQueueFixture : public VulkanFixture {
  protected:
    void makePass(Pass& pass, std::uint32_t capacity, bool prior = false) {
        const auto& ctx = deviceCtx();
        const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, Pass::kPushSize};
        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &range;
        VkPipelineLayout raw{};
        ASSERT_EQ(vkCreatePipelineLayout(ctx.device, &info, nullptr, &raw), VK_SUCCESS);
        layout = harmonia::core::UniquePipelineLayout{ctx.device, raw};
        const std::filesystem::path shaders{HARMONIA_TEST_SHADER_DIR};
        if (prior) {
            auto data = harmonia::core::Buffer::create(ctx,
                                                       16,
                                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                                       VMA_MEMORY_USAGE_GPU_TO_CPU,
                                                       "test.sms.prior.stage");
            ASSERT_TRUE(data.has_value());
            auxiliary = std::move(*data);
            const std::array<std::uint32_t, 4> zero{};
            auxiliary.uploadData(zero.data(), sizeof(zero));
        }
        auto candidate = Pass::create(ctx,
                                      capacity,
                                      layout,
                                      shaders / "sms_queue_control.spv",
                                      shaders / "sms_queue_test.spv",
                                      shaders / "sms_queue_validate.spv",
                                      1,
                                      VK_SHADER_STAGE_COMPUTE_BIT,
                                      prior ? shaders / "sms_queue_prior_test.spv" : std::filesystem::path{},
                                      prior ? auxiliary.deviceAddress() : 0);
        ASSERT_TRUE(candidate.has_value()) << static_cast<int>(candidate.error());
        pass = std::move(*candidate);
    }

    void begin(Pass& pass, std::span<const Record> records) {
        auto cmd = commandPool().beginOneShot();
        ASSERT_TRUE(cmd.has_value());
        if (!records.empty()) {
            ASSERT_LE(records.size_bytes(), 65536U);
            vkCmdUpdateBuffer(*cmd, pass.workBuffer().handle(), 0, records.size_bytes(), records.data());
        }
        ASSERT_EQ(pass.recordBegin(*cmd), VK_SUCCESS);
        ASSERT_EQ(commandPool().endOneShot(*cmd), VK_SUCCESS);
    }

    void batch(Pass& pass) {
        auto cmd = commandPool().beginOneShot();
        ASSERT_TRUE(cmd.has_value());
        ASSERT_EQ(pass.recordBatch(*cmd), VK_SUCCESS);
        ASSERT_EQ(commandPool().endOneShot(*cmd), VK_SUCCESS);
    }

    void expectStatus(const Pass& pass, std::uint32_t expected, std::uint32_t error = 0) {
        const auto actual = pass.completion();
        ASSERT_TRUE(actual.has_value());
        EXPECT_EQ(actual->status, expected);
        EXPECT_EQ(actual->error, error);
    }

    void readWork(const Pass& pass, std::vector<Record>& results) {
        results.resize(pass.capacity());
        auto readback = harmonia::core::Buffer::create(deviceCtx(),
                                                       results.size() * sizeof(Record),
                                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                       VMA_MEMORY_USAGE_GPU_TO_CPU,
                                                       "test.sms.queue.readback");
        ASSERT_TRUE(readback.has_value());
        auto cmd = commandPool().beginOneShot();
        ASSERT_TRUE(cmd.has_value());
        VkMemoryBarrier2 memory{};
        memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        memory.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        memory.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        memory.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        memory.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &memory;
        vkCmdPipelineBarrier2(*cmd, &dependency);
        const VkBufferCopy copy{0, 0, results.size() * sizeof(Record)};
        vkCmdCopyBuffer(*cmd, pass.workBuffer().handle(), readback->handle(), 1, &copy);
        memory.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        memory.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        memory.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        memory.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        vkCmdPipelineBarrier2(*cmd, &dependency);
        ASSERT_EQ(commandPool().endOneShot(*cmd), VK_SUCCESS);
        ASSERT_EQ(readback->readData(results.data(), results.size() * sizeof(Record)), VK_SUCCESS);
    }

    // Declared before pass locals in tests, so pipeline layout outlives pipelines.
    harmonia::core::UniquePipelineLayout layout;
    harmonia::core::Buffer auxiliary;
};
} // namespace

TEST_F(SmsQueueFixture, PendingWorkResumesWithoutDroppedOrRepeatedSlots) {
    Pass pass;
    ASSERT_NO_FATAL_FAILURE(makePass(pass, 129)); // partial third workgroup
    EXPECT_FALSE(pass.completion().has_value());
    std::vector<Record> inputs(pass.capacity());
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        for (std::size_t word = 0; word < inputs[i].size(); ++word)
            inputs[i][word] = static_cast<std::uint32_t>(i * 1000 + word + 17);
        inputs[i][0] = i % 3 == 0 ? 0U : 2U;
        inputs[i][2] = 0;
        inputs[i][4] = inputs[i][5] = inputs[i][6] = 0; // independent atomic visit ledger
        inputs[i][7] = inputs[i][0] == 0 ? 0U : 1U;
    }
    ASSERT_NO_FATAL_FAILURE(begin(pass, inputs));
    for (auto expected : {Completion::Pending, Completion::Pending, Completion::Ready}) {
        ASSERT_NO_FATAL_FAILURE(batch(pass));
        ASSERT_NO_FATAL_FAILURE(expectStatus(pass, expected));
    }
    std::vector<Record> output;
    ASSERT_NO_FATAL_FAILURE(readWork(pass, output));
    ASSERT_EQ(output.size(), inputs.size());
    for (std::size_t i = 0; i < output.size(); ++i) {
        EXPECT_EQ(output[i][0], 0U) << i;
        EXPECT_EQ(output[i][4], 1U) << i;
        EXPECT_EQ(output[i][5], i % 3 == 0 ? 0U : 1U) << i;
        EXPECT_EQ(output[i][6], i % 3 == 0 ? 0U : 1U) << i;
        for (std::size_t word = 1; word < output[i].size(); ++word)
            if (word < 4 || word > 7)
                EXPECT_EQ(output[i][word], inputs[i][word]) << i << ":" << word;
        EXPECT_EQ(output[i][7], inputs[i][0] == 0 ? 0U : 4U);
    }
    // A new frame must reset stale ping-pong counts; no old work can leak in.
    ASSERT_NO_FATAL_FAILURE(begin(pass, std::vector<Record>(inputs.size())));
    ASSERT_NO_FATAL_FAILURE(batch(pass));
    ASSERT_NO_FATAL_FAILURE(expectStatus(pass, Completion::Ready));
    ASSERT_NO_FATAL_FAILURE(readWork(pass, output));
    for (const auto& record : output) {
        EXPECT_EQ(record[4], 1U);
        EXPECT_EQ(record[5], 0U);
        EXPECT_EQ(record[6], 0U);
    }
}

TEST_F(SmsQueueFixture, EmptyWorkIsReadyWithoutExecutingProducer) {
    Pass pass;
    ASSERT_NO_FATAL_FAILURE(makePass(pass, 0));
    ASSERT_NO_FATAL_FAILURE(begin(pass, {}));
    ASSERT_NO_FATAL_FAILURE(batch(pass));
    ASSERT_NO_FATAL_FAILURE(expectStatus(pass, Completion::Ready));
}

TEST_F(SmsQueueFixture, NumericErrorRemainsFailedAcrossContinuation) {
    Pass pass;
    ASSERT_NO_FATAL_FAILURE(makePass(pass, 1));
    std::array<Record, 1> inputs{};
    inputs[0][2] = 1;
    ASSERT_NO_FATAL_FAILURE(begin(pass, inputs));
    for (int i = 0; i < 2; ++i) {
        ASSERT_NO_FATAL_FAILURE(batch(pass));
        ASSERT_NO_FATAL_FAILURE(expectStatus(pass, Completion::Failed, 4));
    }
}

TEST_F(SmsQueueFixture, QueueOverflowReportsFailureInsteadOfDroppingWork) {
    Pass pass;
    ASSERT_NO_FATAL_FAILURE(makePass(pass, 129));
    std::vector<Record> inputs(pass.capacity());
    for (auto& input : inputs)
        input[2] = 2;
    ASSERT_NO_FATAL_FAILURE(begin(pass, inputs));
    ASSERT_NO_FATAL_FAILURE(batch(pass));
    ASSERT_NO_FATAL_FAILURE(expectStatus(pass, Completion::Failed, 1));
    ASSERT_NO_FATAL_FAILURE(batch(pass));
    ASSERT_NO_FATAL_FAILURE(expectStatus(pass, Completion::Failed, 1));
    ASSERT_NO_FATAL_FAILURE(begin(pass, std::vector<Record>(inputs.size())));
    ASSERT_NO_FATAL_FAILURE(batch(pass));
    ASSERT_NO_FATAL_FAILURE(expectStatus(pass, Completion::Ready));
    std::vector<Record> output;
    ASSERT_NO_FATAL_FAILURE(readWork(pass, output));
    for (const auto& record : output)
        EXPECT_EQ(record[4], 1U);
}

TEST_F(SmsQueueFixture, InvalidSlotAndContinuationErrorsCombineAndStayFailed) {
    Pass pass;
    ASSERT_NO_FATAL_FAILURE(makePass(pass, 2));
    std::array<Record, 2> inputs{};
    inputs[0][0] = inputs[1][0] = 2;
    inputs[0][2] = 3; // invalid compacted slot, diagnosed next batch
    inputs[1][2] = 4; // numeric failure first arises in continuation
    ASSERT_NO_FATAL_FAILURE(begin(pass, inputs));
    ASSERT_NO_FATAL_FAILURE(batch(pass));
    ASSERT_NO_FATAL_FAILURE(expectStatus(pass, Completion::Pending));
    for (int i = 0; i < 2; ++i) {
        ASSERT_NO_FATAL_FAILURE(batch(pass));
        ASSERT_NO_FATAL_FAILURE(expectStatus(pass, Completion::Failed, 2U | 4U));
    }
}

TEST_F(SmsQueueFixture, RejectsSignedPointerIndexOverflowBeforeAllocating) {
    Pass pass;
    ASSERT_NO_FATAL_FAILURE(makePass(pass, 0));
    for (auto count : {0x80000000U, 0x80000001U, 0xFFFFFFFFU}) {
        const auto rejected = Pass::create(deviceCtx(), count, layout, "unused.spv", "unused.spv", "unused.spv");
        ASSERT_FALSE(rejected.has_value());
        EXPECT_EQ(rejected.error(), VK_ERROR_INITIALIZATION_FAILED);
    }
}

TEST_F(SmsQueueFixture, DroppedPendingRecordCannotPublishReady) {
    Pass pass;
    ASSERT_NO_FATAL_FAILURE(makePass(pass, 1));
    std::array<Record, 1> inputs{};
    inputs[0][0] = 2;
    inputs[0][2] = 5;
    inputs[0][7] = 1;
    ASSERT_NO_FATAL_FAILURE(begin(pass, inputs));
    ASSERT_NO_FATAL_FAILURE(batch(pass));
    ASSERT_NO_FATAL_FAILURE(expectStatus(pass, Completion::Failed, 2));
}

TEST_F(SmsQueueFixture, PriorRunsAfterPreparationOncePerLogicalFrame) {
    Pass pass;
    ASSERT_NO_FATAL_FAILURE(makePass(pass, 1, true));
    std::array<Record, 1> input{};
    input[0][0] = 2;
    input[0][62] = input[0][63] = 1;
    for (std::uint32_t frame = 1; frame <= 2; ++frame) {
        ASSERT_NO_FATAL_FAILURE(begin(pass, input));
        for (auto expected : {Completion::Pending, Completion::Pending, Completion::Ready}) {
            ASSERT_NO_FATAL_FAILURE(batch(pass));
            ASSERT_NO_FATAL_FAILURE(expectStatus(pass, expected));
        }
        std::array<std::uint32_t, 4> result{};
        ASSERT_EQ(auxiliary.readData(result.data(), sizeof(result)), VK_SUCCESS);
        EXPECT_EQ(result[0], frame) << "Prior must not rebuild during reciprocal continuation";
        EXPECT_EQ(result[1], 1U);
        EXPECT_EQ(result[2], 1U) << "Preparation must precede prior walks";
        EXPECT_EQ(result[3], 2U) << "Main root steps start only after the prior";
    }
}
