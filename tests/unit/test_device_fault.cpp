// VK16 unit tests — the purely-CPU slice of the device_fault forensics feature.
// The probe/feature wiring is device-creation-bound (needs a real Vulkan device)
// and is covered by the integration suite instead; only the dump-artifact name
// contract is testable without a device, so only that is tested here.
#include <cstdint>
#include <gtest/gtest.h>
#include <regex>
#include <string>

#include "harmonia/vulkan_init/DeviceFault.hpp"

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
