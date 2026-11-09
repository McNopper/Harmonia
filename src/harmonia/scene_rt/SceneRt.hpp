#ifndef HARMONIA_SCENE_RT_SCENERT_HPP
#define HARMONIA_SCENE_RT_SCENERT_HPP

#include <volk/volk.h>

#include <cstdint>
#include <vector>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/CommandPool.hpp"
#include "harmonia/rt/AccelerationStructure.hpp"
#include "harmonia/rt/TlasBuilder.hpp"
#include "harmonia/scene/SceneBase.hpp"

namespace harmonia::scene_rt {

/// Stamps the scene's current instance placements into a TLAS instance array
/// (one TLAS instance per scene instance; the per-instance mask comes from
/// SceneBase::instanceMask). Shared by the build, refit and update paths so
/// the stamping cannot drift between them.
[[nodiscard]] std::vector<VkAccelerationStructureInstanceKHR> collectTlasInstances(const scene::SceneBase& scene);

/// Builds the TLAS for a fully uploaded scene (SceneBase::build() has run:
/// scene buffers + one BLAS per unique mesh). One TLAS instance per scene
/// instance; the per-instance mask comes from SceneBase::instanceMask.
/// @p options defaults to the static-scene build; animated scenes pass
/// `allowUpdate` so the TLAS can be refit in place later (PERF3).
[[nodiscard]] VkResult buildTlas(scene::SceneBase& scene,
                                 const DeviceContext& ctx,
                                 const core::CommandPool& pool,
                                 rt::AccelerationStructure& outTlas,
                                 VkDeviceAddress& outAddress,
                                 const rt::TlasBuildOptions& options = {});

/// PERF3: in-place TLAS refit against the scene's current instance
/// transforms. Requires the TLAS to have been built with
/// `TlasBuildOptions::allowUpdate`; the handle and device address stay valid.
[[nodiscard]] VkResult refitTlas(scene::SceneBase& scene,
                                 const DeviceContext& ctx,
                                 const core::CommandPool& pool,
                                 rt::AccelerationStructure& tlas,
                                 const rt::TlasBuildOptions& options);

/// PERF3 (ANI2 seam) — the shared animated-scene TLAS update policy, consumed
/// 1:1 by Hyperion and Theia:
///
/// - No-op (VK_SUCCESS) unless the scene's instances are dirty
///   (SceneBase::instanceTransform / setAnimationTime set the flag).
/// - REFIT when the TLAS was built update-capable and the update counter has
///   not reached @p rebuildInterval — refits are cheap but the BVH tree
///   structure drifts from the true distribution as instances move, so a full
///   rebuild refreshes the structure periodically.
/// - FULL REBUILD when the TLAS is not update-capable, or on every
///   @p rebuildInterval-th update. A rebuild creates a fresh AS — @p outAddress
///   changes and the consumer must rebind it (the renderers' per-frame TLAS
///   descriptor write already does).
///
/// Both paths record their own one-shot submission and host-wait for
/// completion, so any subsequently submitted frame work is ordered after the
/// update. The update counter is owned by the caller (persist it on the
/// scene); it is advanced exactly when GPU work ran.
///
/// @p updateCapable must match the options the TLAS was built with
/// (refit requires the source AS to carry ALLOW_UPDATE).
[[nodiscard]] VkResult updateTlas(scene::SceneBase& scene,
                                  const DeviceContext& ctx,
                                  const core::CommandPool& pool,
                                  rt::AccelerationStructure& tlas,
                                  VkDeviceAddress& outAddress,
                                  bool updateCapable,
                                  std::uint32_t& updateCounter,
                                  std::uint32_t rebuildInterval = 128);

} // namespace harmonia::scene_rt

#endif // HARMONIA_SCENE_RT_SCENERT_HPP
