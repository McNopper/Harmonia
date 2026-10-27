#include "harmonia/vulkan_init/DeviceFault.hpp"

#include <volk/volk.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

#include "harmonia/core/Logger.hpp"

namespace harmonia::vulkan_init {

namespace {

/// Defensive allocation ceiling for the vendor binary blob: a sane driver reports
/// at most a few MiB; anything past 1 GiB is garbage and must not be allocated.
constexpr VkDeviceSize kMaxVendorBinarySize = 1ULL << 30;
constexpr std::uint32_t kMaxFaultRecords = 65536U;

[[nodiscard]] const char* faultAddressTypeLabel(VkDeviceFaultAddressTypeKHR type) noexcept {
    switch (type) {
    case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT:
        return "read-invalid";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT:
        return "write-invalid";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT:
        return "execute-invalid";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT:
        return "instruction-pointer-unknown";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT:
        return "instruction-pointer-invalid";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT:
        return "instruction-pointer-fault";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT:
        return "none";
    default:
        return "unknown";
    }
}

[[nodiscard]] std::string nullTerminated(const char (&field)[VK_MAX_DESCRIPTION_SIZE]) noexcept {
    const char* end = field;
    while (end < field + VK_MAX_DESCRIPTION_SIZE && *end != '\0') {
        ++end;
    }
    return std::string{field, end};
}

/// Dump the raw vendor binary fault blob for vendor-tool analysis. Errors are
/// logged, never thrown — the surrounding device-lost handling must not be masked.
void dumpVendorBinary(const void* data, VkDeviceSize size) noexcept {
    if (data == nullptr || size == 0) {
        return;
    }
    const auto timestamp = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
    std::error_code ec;
    const std::filesystem::path directory = std::filesystem::current_path(ec);
    if (ec) {
        core::Logger::error("DeviceFault: cannot resolve a dump directory ({}) — vendor binary blob not written",
                            ec.message());
        return;
    }
    const std::filesystem::path path = directory / deviceFaultFileName(timestamp);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        core::Logger::error("DeviceFault: failed to open '{}' for the vendor binary fault blob", path.string());
        return;
    }
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    out.close();
    if (!out) {
        core::Logger::error("DeviceFault: short write on '{}' — vendor binary blob may be incomplete", path.string());
        return;
    }
    core::Logger::error("DeviceFault: raw vendor binary fault blob ({} bytes) dumped to '{}' for vendor tool analysis",
                        static_cast<std::uint64_t>(size),
                        path.string());
}

} // namespace

std::string deviceFaultFileName(std::uint64_t epochMillis) noexcept {
    return std::format("device_fault_{}.bin", epochMillis);
}

void reportDeviceFault(const DeviceContext& ctx) noexcept {
    // Diagnosability only: without the enabled extension (or a loaded entry point)
    // there is nothing to retrieve — the caller's device-lost error path continues.
    if (!ctx.deviceFaultSupported || ctx.device == VK_NULL_HANDLE || vkGetDeviceFaultInfoEXT == nullptr) {
        return;
    }

    // Pass 1: retrieve the fault record counts (pFaultInfo == nullptr).
    VkDeviceFaultCountsEXT counts{};
    counts.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;
    if (vkGetDeviceFaultInfoEXT(ctx.device, &counts, nullptr) != VK_SUCCESS) {
        core::Logger::error("DeviceFault: fault count retrieval failed — no fault record available");
        return;
    }

    // Clamp driver-reported counts before allocating (defensive; the device is in
    // an undefined state and the retrieval must never make the situation worse).
    if (counts.vendorBinarySize > kMaxVendorBinarySize) {
        core::Logger::warn("DeviceFault: implausible vendorBinarySize {} — skipping the binary dump",
                           static_cast<std::uint64_t>(counts.vendorBinarySize));
        counts.vendorBinarySize = 0;
    }
    counts.addressInfoCount = std::min(counts.addressInfoCount, kMaxFaultRecords);
    counts.vendorInfoCount = std::min(counts.vendorInfoCount, kMaxFaultRecords);

    // Pass 2: retrieve the fault info into caller-provided storage.
    std::vector<VkDeviceFaultAddressInfoEXT> addressInfos(counts.addressInfoCount);
    std::vector<VkDeviceFaultVendorInfoEXT> vendorInfos(counts.vendorInfoCount);
    std::vector<std::uint8_t> vendorBinary(static_cast<std::size_t>(counts.vendorBinarySize));
    VkDeviceFaultInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
    info.pAddressInfos = counts.addressInfoCount > 0 ? addressInfos.data() : nullptr;
    info.pVendorInfos = counts.vendorInfoCount > 0 ? vendorInfos.data() : nullptr;
    info.pVendorBinaryData = counts.vendorBinarySize > 0 ? vendorBinary.data() : nullptr;
    if (vkGetDeviceFaultInfoEXT(ctx.device, &counts, &info) != VK_SUCCESS) {
        core::Logger::error("DeviceFault: fault info retrieval failed");
        return;
    }

    core::Logger::error("DeviceFault: device-lost fault: '{}'", nullTerminated(info.description));
    for (const VkDeviceFaultAddressInfoEXT& address : addressInfos) {
        core::Logger::error("DeviceFault: fault address {} 0x{:016X} (precision {:#x})",
                            faultAddressTypeLabel(address.addressType),
                            static_cast<std::uint64_t>(address.reportedAddress),
                            static_cast<std::uint64_t>(address.addressPrecision));
    }
    for (const VkDeviceFaultVendorInfoEXT& vendor : vendorInfos) {
        core::Logger::error("DeviceFault: vendor fault code 0x{:016X} data 0x{:016X}: '{}'",
                            static_cast<std::uint64_t>(vendor.vendorFaultCode),
                            static_cast<std::uint64_t>(vendor.vendorFaultData),
                            nullTerminated(vendor.description));
    }

    // VK16: with VK_EXT_device_address_binding_report enabled at device creation, the
    // implementation's address binding records accompany the fault — the reported fault
    // addresses can be correlated with the bind/unbind events logged by the dedicated
    // debug messenger (named resources make the correlation actionable).
    if (ctx.addressBindingReportSupported) {
        core::Logger::error("DeviceFault: address binding records accompany the fault — correlate the fault "
                            "addresses above with the binding-event log (VK_EXT_device_address_binding_report "
                            "was enabled at device creation)");
    }

    dumpVendorBinary(info.pVendorBinaryData, counts.vendorBinarySize);
}

} // namespace harmonia::vulkan_init
