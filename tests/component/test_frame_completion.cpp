#include <cstdint>
#include <cstring>

#include "fixtures/VulkanTestFixture.hpp"
#include "harmonia/app/IFrameCompletion.hpp"

namespace {

class CompletionProbe final : public harmonia::app::IFrameCompletion {
  public:
    explicit CompletionProbe(VmaAllocator allocator) : m_allocator(allocator) {}
    CompletionProbe(const CompletionProbe&) = delete;
    CompletionProbe& operator=(const CompletionProbe&) = delete;
    ~CompletionProbe() override {
        if (m_buffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(m_allocator, m_buffer, m_allocation);
    }
    VkResult create() {
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = 4;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        allocation.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        VmaAllocationInfo mapped{};
        auto result = vmaCreateBuffer(m_allocator, &info, &allocation, &m_buffer, &m_allocation, &mapped);
        if (result != VK_SUCCESS)
            return result;
        m_mapped = mapped.pMappedData;
        if (m_mapped == nullptr)
            return VK_ERROR_MEMORY_MAP_FAILED;
        std::memset(m_mapped, 0, 4);
        return vmaFlushAllocation(m_allocator, m_allocation, 0, VK_WHOLE_SIZE);
    }
    std::expected<bool, VkResult> ready() noexcept override {
        if (statusError)
            return std::unexpected(VK_ERROR_UNKNOWN);
        const auto result = vmaInvalidateAllocation(m_allocator, m_allocation, 0, VK_WHOLE_SIZE);
        if (result != VK_SUCCESS)
            return std::unexpected(result);
        // Only completion status is read. No workgroup/ray count is returned.
        return initiallyReady || *static_cast<const std::uint32_t*>(m_mapped) == 1U;
    }
    VkResult recordContinuation(VkCommandBuffer cmd) noexcept override {
        ++recordings;
        if (recordError)
            return VK_ERROR_UNKNOWN;
        // Synthetic producer: completion is visible only after the THIRD GPU
        // submission finishes, so polling without submit/wait cannot pass.
        vkCmdFillBuffer(cmd, m_buffer, 0, 4, recordings == 3U ? 1U : 0U);
        const VkMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                       .pNext = nullptr,
                                       .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                       .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                       .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                       .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
        const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                          .pNext = nullptr,
                                          .dependencyFlags = 0,
                                          .memoryBarrierCount = 1,
                                          .pMemoryBarriers = &barrier,
                                          .bufferMemoryBarrierCount = 0,
                                          .pBufferMemoryBarriers = nullptr,
                                          .imageMemoryBarrierCount = 0,
                                          .pImageMemoryBarriers = nullptr};
        vkCmdPipelineBarrier2(cmd, &dependency);
        return VK_SUCCESS;
    }
    bool initiallyReady = false;
    bool statusError = false;
    bool recordError = false;
    std::uint32_t recordings = 0;

  private:
    VmaAllocator m_allocator;
    VkBuffer m_buffer{};
    VmaAllocation m_allocation{};
    void* m_mapped{};
};

} // namespace

TEST_F(VulkanFixture, FrameCompletionWaitsForEveryPendingGpuChunk) {
    CompletionProbe work(deviceCtx().allocator);
    ASSERT_EQ(work.create(), VK_SUCCESS);
    EXPECT_EQ(harmonia::app::drainFrameWork(work, commandPool()), VK_SUCCESS);
    EXPECT_EQ(work.recordings, 3U);
    ASSERT_TRUE(work.ready().has_value());
    EXPECT_TRUE(*work.ready());
}

TEST_F(VulkanFixture, FrameCompletionReadyWorkDoesNotSubmitAnotherSample) {
    CompletionProbe work(deviceCtx().allocator);
    ASSERT_EQ(work.create(), VK_SUCCESS);
    work.initiallyReady = true;
    EXPECT_EQ(harmonia::app::drainFrameWork(work, commandPool()), VK_SUCCESS);
    EXPECT_EQ(work.recordings, 0U);
}

TEST_F(VulkanFixture, FrameCompletionErrorsAreNotSuccessfulFrames) {
    CompletionProbe statusFailure(deviceCtx().allocator);
    ASSERT_EQ(statusFailure.create(), VK_SUCCESS);
    statusFailure.statusError = true;
    EXPECT_EQ(harmonia::app::drainFrameWork(statusFailure, commandPool()), VK_ERROR_UNKNOWN);
    EXPECT_EQ(statusFailure.recordings, 0U);
    CompletionProbe recordFailure(deviceCtx().allocator);
    ASSERT_EQ(recordFailure.create(), VK_SUCCESS);
    recordFailure.recordError = true;
    EXPECT_EQ(harmonia::app::drainFrameWork(recordFailure, commandPool()), VK_ERROR_UNKNOWN);
    EXPECT_EQ(recordFailure.recordings, 1U);
}
