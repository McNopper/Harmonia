#pragma once

#include <volk/volk.h>

#include <cstdint>
#include <span>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/CommandPool.hpp"
#include "harmonia/renderer/AccelerationStructure.hpp"

namespace harmonia {

/// Build a top-level acceleration structure (TLAS) from a pre-prepared instance array.
///
/// Each instance's object→world transform + visibility mask must already be stamped by the
/// caller — that is the only divergence between renderers (Hyperion uses the default mask;
/// Theia stamps emissive/transparent/opaque). The TLAS-build mechanics (instance upload,
/// geometry/build-info sizing, scratch allocation + alignment, one-shot device-side build)
/// are shared here so they cannot drift.
///
/// On success, @p outTlas holds the built AS and @p outAddress its device address.
/// Returns VK_SUCCESS or the failing VkResult.
[[nodiscard]] VkResult buildTlas(const DeviceContext& ctx,
                                 const CommandPool& pool,
                                 std::span<const VkAccelerationStructureInstanceKHR> instances,
                                 AccelerationStructure& outTlas,
                                 VkDeviceAddress& outAddress);

} // namespace harmonia
