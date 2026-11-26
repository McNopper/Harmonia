#include "harmonia/scene/SceneBase.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "aether/format/SceneAnimator.hpp"
#include "aether/format/Skinning.hpp"
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

void SceneBase::attachAnimation(aether::SceneDesc&& desc, const std::vector<std::uint32_t>& instanceToDesc) {
    m_animation = std::move(desc);
    m_instanceToDesc = instanceToDesc;
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

void SceneBase::bindMorphMesh(std::uint32_t meshIndex,
                              std::string_view meshName,
                              std::uint32_t subVertexOffset,
                              std::uint32_t subVertexCount) {
    m_pendingMorphBindings.push_back(
        PendingMorphBinding{meshIndex, std::string{meshName}, subVertexOffset, subVertexCount});
}

VkResult SceneBase::buildDeformBuffers(const DeviceContext& ctx, const core::CommandPool& pool) {
    if (m_pendingSkinBindings.empty() && m_pendingMorphBindings.empty()) {
        return VK_SUCCESS; // nothing deforms: no deformation buffers needed
    }
    if ((!m_pendingSkinBindings.empty() && !hasSkins()) || (!m_pendingMorphBindings.empty() && !hasMorphs())) {
        return VK_ERROR_INITIALIZATION_FAILED; // bindings without source: loader contract break
    }
    if (!m_animation.has_value()) {
        return VK_ERROR_INITIALIZATION_FAILED; // animation is optional, but this build needs it
    }
    const std::vector<aether::SkinDesc>& skins = m_animation->skins;

    // ANI4: per-skin base tables (declaration order) - a skin's weight rows
    // span its sub-mesh concatenation and its matrices are shared per skin.
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

    // ANI5: per-declared-mesh morph regions, first-appearance order over
    // desc.morphs (targets contiguous per mesh; a region's per-target stride is
    // that target's delta count - the loader's validateMorphVertexCounts made
    // every target of a mesh equal the imported vertex count).
    std::vector<std::string> morphMeshOrder;
    std::unordered_map<std::string, std::vector<std::uint32_t>> targetsByMesh;
    std::unordered_map<std::string, std::uint32_t> regionBase;   // element offset
    std::unordered_map<std::string, std::uint32_t> regionStride; // elements per target
    std::unordered_map<std::string, std::uint32_t> weightBase;   // float offset
    std::uint32_t deltaEltAcc = 0;
    std::uint32_t weightFloatAcc = 0;
    if (hasMorphs()) {
        for (std::size_t i = 0; i < m_animation->morphs.size(); ++i) {
            const std::string& meshName = m_animation->morphs[i].meshName;
            if (targetsByMesh.find(meshName) == targetsByMesh.end()) {
                morphMeshOrder.push_back(meshName);
                regionBase[meshName] = deltaEltAcc;
                weightBase[meshName] = weightFloatAcc;
                regionStride[meshName] = static_cast<std::uint32_t>(m_animation->morphs[i].deltas.size());
            } else if (static_cast<std::uint32_t>(m_animation->morphs[i].deltas.size()) != regionStride[meshName]) {
                return VK_ERROR_INITIALIZATION_FAILED; // hand-built scene: unequal strides
            }
            targetsByMesh[meshName].push_back(static_cast<std::uint32_t>(i));
            deltaEltAcc += static_cast<std::uint32_t>(m_animation->morphs[i].deltas.size());
            weightFloatAcc += 1;
        }
    }

    // Merged absolute jobs: one per meshIndex (a mesh bound by BOTH halves gets
    // one job carrying skin + morph fields; an unbound half stays zeroed and
    // the kernel skips it by count).
    m_deformJobs.clear();
    m_deformJobs.reserve(m_pendingSkinBindings.size() + m_pendingMorphBindings.size());
    std::unordered_map<std::uint32_t, std::size_t> jobByMesh;
    auto ensureJob = [&](std::uint32_t meshIndex) -> DeformJob& {
        const auto it = jobByMesh.find(meshIndex);
        if (it != jobByMesh.end()) {
            return m_deformJobs[it->second];
        }
        m_deformJobs.push_back(DeformJob{.meshIndex = meshIndex});
        jobByMesh.emplace(meshIndex, m_deformJobs.size() - 1);
        return m_deformJobs.back();
    };

    for (const std::array<std::uint32_t, 4>& pending : m_pendingSkinBindings) {
        const auto [meshIndex, skinIndex, rowOffset, vertexCount] = pending;
        if (skinIndex >= skins.size() || vertexCount == 0) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        DeformJob& job = ensureJob(meshIndex);
        if (job.vertexCount != 0 && job.vertexCount != vertexCount) {
            return VK_ERROR_INITIALIZATION_FAILED; // both halves must agree on the range
        }
        job.vertexCount = vertexCount;
        job.weightRow = baseRow[skinIndex] + rowOffset;
        job.matrixOffset = baseElts[skinIndex];
        job.jointCount = static_cast<std::uint32_t>(skins[skinIndex].joints.size());
        job.useDqs = skins[skinIndex].blend == aether::SkinDesc::Blend::Dqs ? 1U : 0U;
    }
    for (const PendingMorphBinding& pending : m_pendingMorphBindings) {
        const auto baseIt = regionBase.find(pending.meshName);
        if (baseIt == regionBase.end() || pending.subVertexCount == 0 ||
            static_cast<std::uint64_t>(pending.subVertexOffset) + pending.subVertexCount >
                regionStride[pending.meshName]) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        DeformJob& job = ensureJob(pending.meshIndex);
        if (job.vertexCount != 0 && job.vertexCount != pending.subVertexCount) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        job.vertexCount = pending.subVertexCount;
        job.morphDeltaOffset = baseIt->second + pending.subVertexOffset;
        job.morphDeltaStride = regionStride[pending.meshName];
        job.morphTargetCount = static_cast<std::uint32_t>(targetsByMesh[pending.meshName].size());
        job.morphWeightOffset = weightBase[pending.meshName];
    }
    m_pendingSkinBindings.clear();
    m_pendingMorphBindings.clear();

    // Staging groups: one per declared-mesh weight region, in region order -
    // stageMorphWeights re-derives the float offsets by running sum (the same
    // accumulation as weightBase above).
    m_morphWeightGroups.clear();
    for (const std::string& meshName : morphMeshOrder) {
        m_morphWeightGroups.push_back(targetsByMesh[meshName]);
    }

    constexpr VkBufferUsageFlags kDeformBuf = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    // ANI4: skin rows (uint4 indices / float4 weights - 4 entries per vertex,
    // exactly the StructuredBuffer element stride). Skin-free (morph-only)
    // scenes leave these INVALID - the pass binds null descriptors instead.
    if (rowAcc > 0) {
        std::vector<std::uint32_t> allIndices;
        allIndices.reserve(static_cast<std::size_t>(rowAcc) * 4);
        std::vector<float> allWeights;
        allWeights.reserve(static_cast<std::size_t>(rowAcc) * 4);
        for (const aether::SkinDesc& skin : skins) {
            allIndices.insert(allIndices.end(), skin.boneIndices.begin(), skin.boneIndices.end());
            allWeights.insert(allWeights.end(), skin.boneWeights.begin(), skin.boneWeights.end());
        }

        auto indexBuf = uploadStorageBuffer(
            ctx, pool, std::as_bytes(std::span<const std::uint32_t>(allIndices)), "scene.skinIndices", kDeformBuf);
        if (!indexBuf) {
            return indexBuf.error();
        }
        m_skinIndexBuffer = std::move(*indexBuf);

        auto weightBuf = uploadStorageBuffer(
            ctx, pool, std::as_bytes(std::span<const float>(allWeights)), "scene.skinWeights", kDeformBuf);
        if (!weightBuf) {
            return weightBuf.error();
        }
        m_skinWeightBuffer = std::move(*weightBuf);
    } else {
        m_skinMatrixScratch.clear();
    }

    // ANI4: matrix buffer + scratch: zero-filled (a pose must stage every
    // element before any dispatch reads it). TRANSFER_DST from Buffer::create.
    if (eltAcc > 0) {
        const VkDeviceSize matrixBytes = static_cast<VkDeviceSize>(eltAcc) * sizeof(aether::Mat4);
        std::vector<std::byte> zeros(static_cast<std::size_t>(matrixBytes), std::byte{0});
        auto matrixBuf =
            uploadStorageBuffer(ctx, pool, std::span<const std::byte>(zeros), "scene.skinMatrices", kDeformBuf);
        if (!matrixBuf) {
            return matrixBuf.error();
        }
        m_skinMatrixBuffer = std::move(*matrixBuf);
        m_skinMatrixScratch = std::move(zeros);
    } else {
        m_skinMatrixScratch.clear();
    }

    // ANI4-DQS: the joint-major dual-quaternion buffer is allocated ONLY when
    // some skin declares blend = "dqs" (joint-parallel to the matrices - the
    // job's matrixOffset doubles as the DQ joint base; zero-filled until the
    // first stageSkinMatrices, which also extracts the DQs). DQS-only scenes
    // keep the LBS buffers too (staged but unread - the pass binds both).
    {
        bool anyDqs = false;
        for (const aether::SkinDesc& skin : skins) {
            if (skin.blend == aether::SkinDesc::Blend::Dqs) {
                anyDqs = true;
                break;
            }
        }
        if (anyDqs && eltAcc > 0) {
            const VkDeviceSize dqBytes = static_cast<VkDeviceSize>(eltAcc) * 8 * sizeof(float);
            std::vector<std::byte> zeroDq(static_cast<std::size_t>(dqBytes), std::byte{0});
            auto dqBuf =
                uploadStorageBuffer(ctx, pool, std::span<const std::byte>(zeroDq), "scene.skinDqs", kDeformBuf);
            if (!dqBuf) {
                return dqBuf.error();
            }
            m_skinDqBuffer = std::move(*dqBuf);
            m_skinDqScratch = std::move(zeroDq);
        } else {
            m_skinDqScratch.clear();
        }
    }

    // ANI5: pad-w delta rows (Vec3 -> float4 for the 16-byte structured
    // stride), per declared-mesh region in first-appearance order.
    if (hasMorphs()) {
        std::vector<float> deltas;
        deltas.reserve(static_cast<std::size_t>(deltaEltAcc) * 4);
        for (const std::string& meshName : morphMeshOrder) {
            for (const std::uint32_t target : targetsByMesh[meshName]) {
                for (const aether::Vec3& delta : m_animation->morphs[target].deltas) {
                    deltas.push_back(delta.x);
                    deltas.push_back(delta.y);
                    deltas.push_back(delta.z);
                    deltas.push_back(0.0F);
                }
            }
        }
        if (deltas.size() != static_cast<std::size_t>(deltaEltAcc) * 4) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        auto deltaBuf = uploadStorageBuffer(
            ctx, pool, std::as_bytes(std::span<const float>(deltas)), "scene.morphDeltas", kDeformBuf);
        if (!deltaBuf) {
            return deltaBuf.error();
        }
        m_morphDeltaBuffer = std::move(*deltaBuf);

        // Per-tick weight stage (TRANSFER_DST from create) + zero-filled scratch.
        const VkDeviceSize weightBytes = static_cast<VkDeviceSize>(weightFloatAcc) * sizeof(float);
        std::vector<std::byte> zeroWeights(static_cast<std::size_t>(weightBytes), std::byte{0});
        auto morphWeightBuf =
            uploadStorageBuffer(ctx, pool, std::span<const std::byte>(zeroWeights), "scene.morphWeights", kDeformBuf);
        if (!morphWeightBuf) {
            return morphWeightBuf.error();
        }
        m_morphWeightBuffer = std::move(*morphWeightBuf);
        m_morphWeightScratch = std::move(zeroWeights);
    } else {
        m_morphWeightScratch.clear();
    }
    return VK_SUCCESS;
}

VkResult SceneBase::stageSkinMatrices(const std::vector<aether::SkinPose>& poses) {
    if (!m_animation.has_value() || m_animation->skins.empty()) {
        return VK_SUCCESS;
    }
    const std::vector<aether::SkinDesc>& skins = m_animation.value().skins;
    if (m_skinMatrixScratch.empty()) {
        return VK_ERROR_INITIALIZATION_FAILED; // build() ran no buildSkinBuffers
    }

    std::size_t offset = 0;
    std::size_t dqOffset = 0;
    const bool stageDq = !m_skinDqScratch.empty();
    for (const aether::SkinDesc& skin : skins) {
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
        if (stageDq) {
            // ANI4-DQS: extract the joint dual quaternions from the SAME pose
            // (shared aether::skinDualQuaternions - CPU reference and staging
            // are one code path, so the GPU blends identical values).
            const auto dqs = aether::skinDualQuaternions(poseIt->skinMatrices);
            const std::size_t dqBytes = skin.joints.size() * 8 * sizeof(float);
            if (!dqs || dqs->size() != skin.joints.size() * 8 || dqOffset + dqBytes > m_skinDqScratch.size()) {
                harmonia::core::Logger::error("SceneBase: DQS extraction failed for skin '{}'", skin.nodeId);
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            std::memcpy(m_skinDqScratch.data() + dqOffset, dqs->data(), dqBytes);
            dqOffset += dqBytes;
        }
    }
    if (offset != m_skinMatrixScratch.size() || (stageDq && dqOffset != m_skinDqScratch.size())) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return VK_SUCCESS;
}

std::vector<float> SceneBase::evaluateMorphWeights(float seconds) const {
    if (!m_animation.has_value() || m_animation->morphs.empty()) {
        return {};
    }
    return aether::SceneAnimator::evaluateMorphWeights(m_animation.value(), seconds);
}

VkResult SceneBase::stageMorphWeights(const std::vector<float>& weights) {
    if (!m_animation.has_value() || m_animation->morphs.empty()) {
        return VK_SUCCESS;
    }
    if (m_morphWeightScratch.empty() || weights.size() != m_animation->morphs.size()) {
        return VK_ERROR_INITIALIZATION_FAILED; // build ran no morph staging / non-parallel weights
    }
    auto* dst = reinterpret_cast<float*>(m_morphWeightScratch.data());
    const std::size_t totalFloats = m_morphWeightScratch.size() / sizeof(float);
    std::size_t offset = 0;
    for (const std::vector<std::uint32_t>& group : m_morphWeightGroups) {
        if (offset + group.size() > totalFloats) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        for (std::size_t k = 0; k < group.size(); ++k) {
            if (group[k] >= weights.size()) {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            dst[offset + k] = weights[group[k]];
        }
        offset += group.size();
    }
    if (offset != totalFloats) {
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
    // ANI4+ANI5: materialize the merged deformation jobs + upload the skin and
    // morph buffers before any renderer-side buffer references them
    // (DeformPass::create reads them right after build).
    if (const VkResult deformResult = buildDeformBuffers(ctx, pool); deformResult != VK_SUCCESS) {
        return deformResult;
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
