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
