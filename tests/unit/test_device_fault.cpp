// VK16 unit tests — the purely-CPU slice of the device_fault forensics feature.
// The probe/feature wiring is device-creation-bound (needs a real Vulkan device)
// and is covered by the integration suite instead; only the dump-artifact name
// contract is testable without a device, so only that is tested here.
//
// Also the device limit the selection requires for the renderers' push-constant
// block, with simulated limits: the selection itself needs a device.
#include <cstdint>
#include <gtest/gtest.h>
#include <regex>
#include <string>

#include "harmonia/vulkan_init/DeviceFault.hpp"
#include "harmonia/vulkan_init/PhysicalDevice.hpp"

namespace {

/// The limits of a device whose push-constant limit is @p bytes.
[[nodiscard]] VkPhysicalDeviceLimits withPushConstants(std::uint32_t bytes) noexcept {
    VkPhysicalDeviceLimits limits{};
    limits.maxPushConstantsSize = bytes;
    return limits;
}

} // namespace

// The renderers' main pass pushes a 256-byte block; Vulkan guarantees only 128.
TEST(DevicePushConstants, RequirementIsTheRenderersBlock) {
    constexpr auto requirement = harmonia::vulkan_init::kRequiredPushConstantsSize;
    EXPECT_EQ(requirement, 256U);
    EXPECT_GT(requirement, 128U) << "twice the minimum Vulkan guarantees";
}

TEST(DevicePushConstants, ALimitBelowTheBlockIsRejected) {
    EXPECT_FALSE(harmonia::vulkan_init::supportsPushConstants(withPushConstants(0U)));
    EXPECT_FALSE(harmonia::vulkan_init::supportsPushConstants(withPushConstants(128U)))
        << "the minimum Vulkan guarantees is not enough";
    EXPECT_FALSE(harmonia::vulkan_init::supportsPushConstants(withPushConstants(255U)));
    EXPECT_FALSE(harmonia::vulkan_init::supportsPushConstants(VkPhysicalDeviceLimits{}))
        << "a device that reports nothing";
}

TEST(DevicePushConstants, TheBlockAndMoreAreAccepted) {
    EXPECT_TRUE(harmonia::vulkan_init::supportsPushConstants(withPushConstants(256U)));
    EXPECT_TRUE(harmonia::vulkan_init::supportsPushConstants(withPushConstants(256U + 128U)));
}

TEST(DeviceFaultFileName, MatchesTheDocumentedArtifactContract) {
    // VK16 contract: `device_fault_<timestamp>.bin` — the file vendor fault-analysis
    // tools are pointed at; scripts and docs depend on this exact shape.
    const std::string name = harmonia::vulkan_init::deviceFaultFileName(1698401234567ULL);
    const std::regex pattern{"device_fault_[0-9]+\\.bin"};
    EXPECT_TRUE(std::regex_match(name, pattern)) << name;
}

TEST(DeviceFaultFileName, EmbedsTheTimestampVerbatim) {
    EXPECT_EQ(harmonia::vulkan_init::deviceFaultFileName(0ULL), "device_fault_0.bin");
    EXPECT_EQ(harmonia::vulkan_init::deviceFaultFileName(1234567890123ULL), "device_fault_1234567890123.bin");
}

TEST(DeviceFaultFileName, IsMonotonicForMonotonicTimestamps) {
    // Later dumps must sort after earlier ones, so a directory of blobs reads in
    // chronological order.
    const std::string earlier = harmonia::vulkan_init::deviceFaultFileName(1000000000000ULL);
    const std::string later = harmonia::vulkan_init::deviceFaultFileName(2000000000000ULL);
    EXPECT_LT(earlier, later);
}
