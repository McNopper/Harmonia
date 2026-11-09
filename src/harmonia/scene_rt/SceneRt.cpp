#include "harmonia/scene_rt/SceneRt.hpp"

#include <cstdint>
#include <vector>

#include "harmonia/rt/TlasBuilder.hpp"

namespace harmonia::scene_rt {

std::vector<VkAccelerationStructureInstanceKHR> collectTlasInstances(const scene::SceneBase& scene) {
    const auto& instances = scene.instances();
    const auto& meshes = scene.meshes();
    std::vector<VkAccelerationStructureInstanceKHR> tlasInstances(instances.size());
    for (std::size_t i = 0; i < instances.size(); ++i) {
        const harmonia::scene::InstanceRecord& inst = instances[i];
        tlasInstances[i] = meshes[inst.meshIndex]->makeInstance(static_cast<std::uint32_t>(i), inst.transform);
        tlasInstances[i].mask = scene.instanceMask(i);
    }
    return tlasInstances;
}

VkResult buildTlas(scene::SceneBase& scene,
                   const DeviceContext& ctx,
                   const core::CommandPool& pool,
                   rt::AccelerationStructure& outTlas,
                   VkDeviceAddress& outAddress,
                   const rt::TlasBuildOptions& options) {
    const std::vector<VkAccelerationStructureInstanceKHR> tlasInstances = collectTlasInstances(scene);
    return rt::buildTlas(ctx, pool, tlasInstances, options, outTlas, outAddress);
}

VkResult refitTlas(scene::SceneBase& scene,
                   const DeviceContext& ctx,
                   const core::CommandPool& pool,
                   rt::AccelerationStructure& tlas,
                   const rt::TlasBuildOptions& options) {
    const std::vector<VkAccelerationStructureInstanceKHR> tlasInstances = collectTlasInstances(scene);
    return rt::refitTlas(ctx, pool, tlasInstances, options, tlas);
}

VkResult updateTlas(scene::SceneBase& scene,
                    const DeviceContext& ctx,
                    const core::CommandPool& pool,
                    rt::AccelerationStructure& tlas,
                    VkDeviceAddress& outAddress,
                    bool updateCapable,
                    std::uint32_t& updateCounter,
                    std::uint32_t rebuildInterval) {
    if (!scene.instancesDirty()) {
        return VK_SUCCESS;
    }

    const rt::TlasBuildOptions options{.allowUpdate = updateCapable};
    // First update refits (the common animated-frame path); every
    // rebuildInterval-th update afterwards, and every update of a
    // non-update-capable TLAS, is a full rebuild.
    const bool rebuild = !updateCapable || (updateCounter > 0 && updateCounter % rebuildInterval == 0);

    const VkResult result =
        rebuild ? buildTlas(scene, ctx, pool, tlas, outAddress, options) : refitTlas(scene, ctx, pool, tlas, options);
    if (result == VK_SUCCESS) {
        ++updateCounter;
        scene.clearInstancesDirty();
    }
    return result;
}

} // namespace harmonia::scene_rt
