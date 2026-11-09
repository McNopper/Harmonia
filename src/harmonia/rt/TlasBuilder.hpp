#ifndef HARMONIA_RENDERER_TLASBUILDER_HPP
#define HARMONIA_RENDERER_TLASBUILDER_HPP

#include <volk/volk.h>

#include <cstdint>
#include <span>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/CommandPool.hpp"
#include "harmonia/rt/AccelerationStructure.hpp"

namespace harmonia::rt {

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
                                 const core::CommandPool& pool,
                                 std::span<const VkAccelerationStructureInstanceKHR> instances,
                                 AccelerationStructure& outTlas,
                                 VkDeviceAddress& outAddress);

/// Build options for the PERF3 slice: dynamic scenes trade build quality for
/// updatability. `allowUpdate` is the prerequisite for `refitTlas`; `allowCompaction`
/// is the prerequisite for compacting the built AS (memory savings); `preferFastBuild`
/// swaps the trace/build preference for per-frame rebuilds.
struct TlasBuildOptions {
    bool allowUpdate = false;
    bool allowCompaction = false;
    bool preferFastBuild = false;
};

/// Options overload — same build, with the PERF3 flags.
[[nodiscard]] VkResult buildTlas(const DeviceContext& ctx,
                                 const core::CommandPool& pool,
                                 std::span<const VkAccelerationStructureInstanceKHR> instances,
                                 const TlasBuildOptions& options,
                                 AccelerationStructure& outTlas,
                                 VkDeviceAddress& outAddress);

/// In-place REFIT of a TLAS previously built with `TlasBuildOptions::allowUpdate`:
/// records an update-mode build (src = dst = the existing AS) against the new
/// instance transforms. The instance array must match the original build's count
/// and custom indices exactly — only transforms may change — and `options` must
/// match the original build options (the update build's flags must equal the
/// source build's flags). Refit does not recreate the AS: the handle and device
/// address stay valid, existing descriptor writes keep working.
[[nodiscard]] VkResult refitTlas(const DeviceContext& ctx,
                                 const core::CommandPool& pool,
                                 std::span<const VkAccelerationStructureInstanceKHR> instances,
                                 const TlasBuildOptions& options,
                                 AccelerationStructure& tlas);

} // namespace harmonia::rt
#endif // HARMONIA_RENDERER_TLASBUILDER_HPP
