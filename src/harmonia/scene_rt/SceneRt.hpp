#ifndef HARMONIA_SCENE_RT_SCENERT_HPP
#define HARMONIA_SCENE_RT_SCENERT_HPP

#include <volk/volk.h>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/CommandPool.hpp"
#include "harmonia/rt/AccelerationStructure.hpp"
#include "harmonia/scene/SceneBase.hpp"

namespace harmonia::scene_rt {

/// Builds the TLAS for a fully uploaded scene (SceneBase::build() has run:
/// scene buffers + one BLAS per unique mesh). One TLAS instance per scene
/// instance; the per-instance mask comes from SceneBase::instanceMask.
[[nodiscard]] VkResult buildTlas(harmonia::scene::SceneBase& scene,
                                 const DeviceContext& ctx,
                                 const core::CommandPool& pool,
                                 rt::AccelerationStructure& outTlas,
                                 VkDeviceAddress& outAddress);

} // namespace harmonia::scene_rt

#endif // HARMONIA_SCENE_RT_SCENERT_HPP
