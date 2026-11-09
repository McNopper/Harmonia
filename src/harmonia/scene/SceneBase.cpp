#include "harmonia/scene/SceneBase.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "aether/format/SceneAnimator.hpp"
#include "harmonia/GpuTypes.hpp"
#include "harmonia/scene/SceneLoader.hpp"

namespace harmonia::scene {

std::uint32_t SceneBase::addMaterial(Material&& mat) {
    m_materials.push_back(mat);
    return static_cast<std::uint32_t>(m_materials.size() - 1);
}

std::uint32_t SceneBase::addTexture(Texture&& texture) {
    const auto idx = static_cast<std::uint32_t>(m_textures.size());
    m_textures.push_back(std::move(texture));
    return idx;
}

std::uint32_t SceneBase::addInstance(std::uint32_t meshIndex, const Transform& transform, std::uint32_t materialIdx) {
    const std::uint32_t idx = static_cast<std::uint32_t>(m_instances.size());
    m_instances.push_back(InstanceRecord{.meshIndex = meshIndex, .transform = transform, .materialIndex = materialIdx});
    return idx;
}

void SceneBase::attachAnimation(aether::SceneDesc&& desc, std::vector<std::uint32_t> instanceToDesc) {
    m_animation = std::move(desc);
    m_instanceToDesc = std::move(instanceToDesc);
}

void SceneBase::setAnimationTime(float seconds) {
    if (!m_animation) {
        return;
    }
    // Aether owns the evaluation (ANI1): tracks are sampled and the node
    // compounding re-run; the renderers only stamp the resolved TRS through the
    // load-time mapping. Stamping is dirty regardless of whether any track
    // actually changed a value at @p seconds — the TLAS update path decides
    // whether GPU work is worth it.
    const aether::SceneDesc evaluated = aether::SceneAnimator::evaluate(*m_animation, seconds);
    for (std::size_t i = 0; i < m_instances.size(); ++i) {
        const std::uint32_t descIndex = m_instanceToDesc[i];
        if (descIndex >= evaluated.instances.size()) {
            continue;
        }
        m_instances[i].transform = toTransform(evaluated.instances[descIndex]);
    }
    m_instancesDirty = true;
}

std::uint32_t SceneBase::addLight(std::unique_ptr<Light> light) {
    const std::uint32_t index = static_cast<std::uint32_t>(m_lights.size());
    m_lights.push_back(std::move(light));
    return index;
}

std::uint32_t SceneBase::addMesh(const DeviceContext& ctx,
                                 const core::CommandPool& pool,
                                 MeshData&& data,
                                 const MeshOpacity& opacity,
                                 std::string_view name) {
    const std::uint32_t meshIndex = static_cast<std::uint32_t>(m_meshes.size());
    const std::string debugName = name.empty() ? std::string{"mesh."} + std::to_string(meshIndex) : std::string{name};

    auto mesh = createTriangleMesh(ctx, pool, std::move(data), opacity, debugName);
    if (!mesh) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    m_meshes.push_back(std::move(mesh));
    return meshIndex;
}

const aether::OpacityMicromapData& SceneBase::addOpacityMicromap(aether::OpacityMicromapData&& data) {
    m_ommAssets.push_back(std::move(data));
    return m_ommAssets.back();
}

VkResult SceneBase::build(const DeviceContext& ctx, const core::CommandPool& pool) {
    if (m_instances.empty()) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (const VkResult result = buildSceneBuffers(ctx, pool); result != VK_SUCCESS) {
        return result;
    }
    // One BLAS per unique mesh; N TLAS instances reference a shared BLAS.
    for (auto& mesh : m_meshes) {
        if (const VkResult result = mesh->buildBlas(ctx, pool); result != VK_SUCCESS) {
            return result;
        }
    }
    return finishBuild(ctx, pool);
}

std::uint32_t SceneBase::instanceMask(std::size_t /*instanceIndex*/) const {
    return kInstanceMaskAll;
}

std::expected<core::Buffer, VkResult> SceneBase::uploadStorageBuffer(const DeviceContext& ctx,
                                                                     const core::CommandPool& pool,
                                                                     std::span<const std::byte> data,
                                                                     std::string_view debugName,
                                                                     VkBufferUsageFlags usage) {
    return core::Buffer::upload(ctx, pool, data, usage, debugName);
}

} // namespace harmonia::scene
