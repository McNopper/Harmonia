#include "harmonia/scene/SceneBase.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include "aether/format/SceneAnimator.hpp"
#include "harmonia/GpuTypes.hpp"
#include "harmonia/core/Logger.hpp"
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

void SceneBase::markMeshDynamic(std::uint32_t meshIndex) {
    if (meshIndex < m_meshes.size()) {
        m_meshes[meshIndex]->setDynamic(true);
    }
}

void SceneBase::bindSkinMesh(std::uint32_t meshIndex,
                             std::uint32_t skinIndex,
                             std::uint32_t weightRowOffset,
                             std::uint32_t vertexCount) {
    m_pendingSkinBindings.push_back({meshIndex, skinIndex, weightRowOffset, vertexCount});
}

VkResult SceneBase::buildSkinBuffers(const DeviceContext& ctx, const core::CommandPool& pool) {
    if (m_pendingSkinBindings.empty()) {
        return VK_SUCCESS; // nothing a skin covers: no skin buffers needed
    }
    if (!hasSkins()) {
        return VK_ERROR_INITIALIZATION_FAILED; // bindings without skins: loader contract break
    }
    const std::vector<aether::SkinDesc>& skins = m_animation->skins;

    // Per-skin base tables (declaration order): a skin's weight rows span its
    // sub-mesh concatenation and its matrices are shared by every job it drives.
    std::vector<std::uint32_t> baseRow(skins.size(), 0);
    std::vector<std::uint32_t> baseElts(skins.size(), 0);
    std::uint32_t rowAcc = 0;
    std::uint32_t eltAcc = 0;
    for (std::size_t i = 0; i < skins.size(); ++i) {
        baseRow[i] = rowAcc;
        rowAcc += static_cast<std::uint32_t>(skins[i].boneIndices.size() / 4);
        baseElts[i] = eltAcc;
        eltAcc += static_cast<std::uint32_t>(skins[i].joints.size());
    }

    // Absolute jobs from the loader's within-skin offsets.
    m_skinJobs.clear();
    m_skinJobs.reserve(m_pendingSkinBindings.size());
    for (const std::array<std::uint32_t, 4>& pending : m_pendingSkinBindings) {
        const auto [meshIndex, skinIndex, rowOffset, vertexCount] = pending;
        if (skinIndex >= skins.size() || vertexCount == 0) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        m_skinJobs.push_back(SkinJob{
            .meshIndex = meshIndex,
            .vertexCount = vertexCount,
            .weightRow = baseRow[skinIndex] + rowOffset,
            .matrixOffset = baseElts[skinIndex],
            .jointCount = static_cast<std::uint32_t>(skins[skinIndex].joints.size()),
        });
    }
    m_pendingSkinBindings.clear();

    // Concatenated GPU rows (uint4 indices / float4 weights - 4 entries per
    // vertex per row, exactly the buffers' StructuredBuffer element stride).
    std::vector<std::uint32_t> allIndices;
    allIndices.reserve(static_cast<std::size_t>(rowAcc) * 4);
    std::vector<float> allWeights;
    allWeights.reserve(static_cast<std::size_t>(rowAcc) * 4);
    for (const aether::SkinDesc& skin : skins) {
        allIndices.insert(allIndices.end(), skin.boneIndices.begin(), skin.boneIndices.end());
        allWeights.insert(allWeights.end(), skin.boneWeights.begin(), skin.boneWeights.end());
    }

    auto indexBuf = uploadStorageBuffer(ctx,
                                        pool,
                                        std::as_bytes(std::span<const std::uint32_t>(allIndices)),
                                        "scene.skinIndices",
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if (!indexBuf) {
        return indexBuf.error();
    }
    m_skinIndexBuffer = std::move(*indexBuf);

    auto weightBuf =
        uploadStorageBuffer(ctx,
                            pool,
                            std::as_bytes(std::span<const float>(allWeights)),
                            "scene.skinWeights",
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if (!weightBuf) {
        return weightBuf.error();
    }
    m_skinWeightBuffer = std::move(*weightBuf);

    // Matrix buffer + scratch: zero-filled (a pose must stage every element
    // before any dispatch reads it). TRANSFER_DST is added by Buffer::create.
    const VkDeviceSize matrixBytes = static_cast<VkDeviceSize>(eltAcc) * sizeof(aether::Mat4);
    std::vector<std::byte> zeros(static_cast<std::size_t>(matrixBytes), std::byte{0});
    auto matrixBuf =
        uploadStorageBuffer(ctx,
                            pool,
                            std::span<const std::byte>(zeros),
                            "scene.skinMatrices",
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if (!matrixBuf) {
        return matrixBuf.error();
    }
    m_skinMatrixBuffer = std::move(*matrixBuf);
    m_skinMatrixScratch = std::move(zeros);
    return VK_SUCCESS;
}

VkResult SceneBase::stageSkinMatrices(const std::vector<aether::SkinPose>& poses) {
    if (!hasSkins()) {
        return VK_SUCCESS;
    }
    const std::vector<aether::SkinDesc>& skins = m_animation->skins;
    if (m_skinMatrixScratch.empty()) {
        return VK_ERROR_INITIALIZATION_FAILED; // build() ran no buildSkinBuffers
    }

    std::size_t offset = 0;
    for (std::size_t i = 0; i < skins.size(); ++i) {
        const aether::SkinDesc& skin = skins[i];
        const std::size_t bytes = skin.joints.size() * sizeof(aether::Mat4);
        const auto poseIt = std::find_if(
            poses.begin(), poses.end(), [&](const aether::SkinPose& pose) { return pose.nodeId == skin.nodeId; });
        if (poseIt == poses.end() || poseIt->skinMatrices.size() != skin.joints.size()) {
            harmonia::core::Logger::error("SceneBase: no evaluated pose for skin '{}'", skin.nodeId);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (offset + bytes > m_skinMatrixScratch.size()) {
            return VK_ERROR_INITIALIZATION_FAILED; // staging/buffer size diverged
        }
        std::memcpy(m_skinMatrixScratch.data() + offset, poseIt->skinMatrices.data(), bytes);
        offset += bytes;
    }
    if (offset != m_skinMatrixScratch.size()) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return VK_SUCCESS;
}

VkResult SceneBase::refitDynamicBlases(const DeviceContext& ctx, const core::CommandPool& pool) {
    for (const std::unique_ptr<Geometry>& mesh : m_meshes) {
        if (mesh != nullptr && mesh->isDynamic()) {
            if (const VkResult r = mesh->refitBlas(ctx, pool); r != VK_SUCCESS) {
                return r;
            }
        }
    }
    return VK_SUCCESS;
}

std::vector<aether::SkinPose> SceneBase::evaluateSkins(float seconds) const {
    if (!hasSkins()) {
        return {};
    }
    return aether::SceneAnimator::evaluateSkins(*m_animation, seconds);
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
    // ANI4: materialize the skin jobs + upload the weight/matrix buffers before
    // any renderer-side buffer references them (SkinDeformPass::create reads
    // them right after build).
    if (const VkResult skinResult = buildSkinBuffers(ctx, pool); skinResult != VK_SUCCESS) {
        return skinResult;
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
