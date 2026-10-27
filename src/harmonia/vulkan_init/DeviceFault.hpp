#ifndef HARMONIA_VULKAN_INIT_DEVICEFAULT_HPP
#define HARMONIA_VULKAN_INIT_DEVICEFAULT_HPP

#include <string>

#include "harmonia/DeviceContext.hpp"

namespace harmonia::vulkan_init {

/// Artifact name contract (VK16) for a dumped raw vendor binary fault blob:
/// `device_fault_<timestamp>.bin` with a millisecond Unix-epoch timestamp —
/// the file a vendor fault-analysis tool consumes. Pure function, no I/O.
[[nodiscard]] std::string deviceFaultFileName(std::uint64_t epochMillis) noexcept;

/// Device-lost forensics (VK16: VK_EXT_device_fault + VK_EXT_device_address_binding_report).
///
/// Retrieve and report the pending device fault via `vkGetDeviceFaultInfoEXT`: log the fault
/// description, the reported fault addresses and the vendor fault entries, and dump the raw
/// vendor binary blob next to the run's artifacts as `device_fault_<timestamp>.bin` (current
/// working directory) for offline vendor-tool analysis.
///
/// Pure diagnosability: call once from a device-lost surface point (acquire / submit /
/// present). It never changes rendering behavior, never throws, and every internal failure is
/// logged and swallowed so the original VK_ERROR_DEVICE_LOST error stays the authoritative
/// outcome of the surrounding code. No-op unless the device was created with the fault
/// extensions enabled (DeviceContext::deviceFaultSupported).
void reportDeviceFault(const DeviceContext& ctx) noexcept;

} // namespace harmonia::vulkan_init

#endif // HARMONIA_VULKAN_INIT_DEVICEFAULT_HPP
