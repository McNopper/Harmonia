// Component tests: harmonia::core::CommandPool one-shot command buffer submission.
//
// Tests verify:
//   - harmonia::core::CommandPool handle is valid after creation.
//   - beginOneShot / endOneShot round-trip submits successfully.
//   - GPU-side work (vkCmdFillBuffer) actually runs: data written to a
//     host-visible buffer is verified after the fence wait.

#include <volk/volk.h>

#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>

#include "fixtures/VulkanFaultScope.hpp"
#include "fixtures/VulkanTestFixture.hpp"
#include "harmonia/core/Buffer.hpp"
#include "harmonia/core/CommandPool.hpp"

TEST_F(VulkanFixture, CommandPool_HandleNonNull) {
    EXPECT_NE(commandPool().handle(), VK_NULL_HANDLE);
}

// Record a vkCmdFillBuffer command in a one-shot command buffer, submit it,
// wait for completion, then read back the result and verify every DWORD.
TEST_F(VulkanFixture, CommandPool_OneShotFillBufferAndVerify) {
    constexpr std::uint32_t kPattern = 0xDEADBEEFu;
    constexpr VkDeviceSize kSize = 256;

    // Host-visible target: vkCmdFillBuffer writes to it; we read via mappedData().
    auto buf = harmonia::core::Buffer::create(
        deviceCtx(), kSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST, "test.cmdpool.fill");
    ASSERT_TRUE(buf.has_value()) << "VkResult=" << static_cast<int>(buf.error());
    ASSERT_NE(buf->mappedData(), nullptr);

    // Zero the buffer first so we know the fill actually ran.
    std::memset(buf->mappedData(), 0, static_cast<std::size_t>(kSize));

    auto cmd = commandPool().beginOneShot();
    ASSERT_TRUE(cmd.has_value()) << "VkResult=" << static_cast<int>(cmd.error());

    vkCmdFillBuffer(*cmd, buf->handle(), 0, kSize, kPattern);

    ASSERT_EQ(commandPool().endOneShot(*cmd), VK_SUCCESS);

    // After fence wait (inside endOneShot), host-coherent memory is visible.
    const auto* data = static_cast<const std::uint32_t*>(buf->mappedData());
    const std::size_t dwordCount = static_cast<std::size_t>(kSize) / sizeof(std::uint32_t);
    for (std::size_t i = 0; i < dwordCount; ++i) {
        EXPECT_EQ(data[i], kPattern) << "mismatch at DWORD " << i;
    }
}

// Ensure that beginOneShot / endOneShot can be called multiple times in sequence
// (pool is not exhausted, fences are properly cleaned up).
TEST_F(VulkanFixture, CommandPool_MultipleOneShotCallsSucceed) {
    for (int iter = 0; iter < 4; ++iter) {
        auto cmd = commandPool().beginOneShot();
        ASSERT_TRUE(cmd.has_value()) << "iter " << iter << ": " << static_cast<int>(cmd.error());
        EXPECT_EQ(commandPool().endOneShot(*cmd), VK_SUCCESS) << "iter " << iter;
    }
}

TEST_F(VulkanFixture, CommandPool_WaitFailureRetiresBeforeFreeingResources) {
    auto buffer = harmonia::core::Buffer::create(
        deviceCtx(), 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST, "test.wait.failure");
    ASSERT_TRUE(buffer.has_value());
    const auto cmd = commandPool().beginOneShot();
    ASSERT_TRUE(cmd.has_value());
    vkCmdFillBuffer(*cmd, buffer->handle(), 0, 16, 0x12345678U);
    VulkanFaultScope faults;
    faults.failWaitOnce = true;
    faults.idleAllocationFailures = 1;
    faults.arm();
    EXPECT_EQ(commandPool().endOneShot(*cmd), VK_ERROR_OUT_OF_HOST_MEMORY);
    EXPECT_TRUE(faults.waitInjected);
    EXPECT_GE(faults.idleCalls, 2U);
    EXPECT_TRUE(faults.retiredBySubject);
    EXPECT_FALSE(faults.unsafeSemaphoreDestruction);
    EXPECT_FALSE(faults.unsafeCommandFree);
    EXPECT_FALSE(faults.testRecoveryUsed);
}
