#include "harmonia/scene_rt/SceneRt.hpp"

#include <cstdint>
#include <vector>

#include "harmonia/rt/TlasBuilder.hpp"

namespace harmonia::scene_rt {

VkResult buildTlas(harmonia::scene::SceneBase& scene,
                   const DeviceContext& ctx,
                   const core::CommandPool& pool,
                   rt::AccelerationStructure& outTlas,
                   VkDeviceAddress& outAddress) {
    const auto& instances = scene.instances();
    const auto& meshes = scene.meshes();
    std::vector<VkAccelerationStructureInstanceKHR> tlasInstances(instances.size());
    for (std::size_t i = 0; i < instances.size(); ++i) {
        const harmonia::scene::InstanceRecord& inst = instances[i];
        tlasInstances[i] = meshes[inst.meshIndex]->makeInstance(static_cast<std::uint32_t>(i), inst.transform);
        tlasInstances[i].mask = scene.instanceMask(i);
    }
    return rt::buildTlas(ctx, pool, tlasInstances, outTlas, outAddress);
}

} // namespace harmonia::scene_rt
