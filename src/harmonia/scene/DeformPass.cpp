#include "harmonia/scene/DeformPass.hpp"

#include <volk/volk.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include "harmonia/GpuTypes.hpp"
#include "harmonia/core/Logger.hpp"
#include "harmonia/core/PipelineBinaryCache.hpp"
#include "harmonia/core/ShaderModule.hpp"

namespace harmonia::scene {

namespace {

/// Must match the `DeformPush` block of deform.comp.slang (std430, 36 B).
struct DeformPush {
    std::uint32_t vertexCount = 0;
    std::uint32_t sceneVertexOffset = 0;
    std::uint32_t weightRow = 0;
    std::uint32_t matrixOffset = 0;
    std::uint32_t jointCount = 0;
    std::uint32_t morphDeltaOffset = 0;
    std::uint32_t morphDeltaStride = 0;
    std::uint32_t morphTargetCount = 0;
    std::uint32_t morphWeightOffset = 0;
};
static_assert(sizeof(DeformPush) == 36);

/// vkCmdPipelineBarrier2 over buffer barriers only (the pass's barriers are
/// all buffer-scope: transfer/compute/as-build/shader on vertex + staging data).
void bufferBarrier(VkCommandBuffer cmd, std::span<const VkBufferMemoryBarrier2> bars) noexcept {
    const VkDependencyInfo dep{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext = nullptr,
        .dependencyFlags = 0,
        .memoryBarrierCount = 0,
        .pMemoryBarriers = nullptr,
        .bufferMemoryBarrierCount = static_cast<std::uint32_t>(bars.size()),
        .pBufferMemoryBarriers = bars.data(),
        .imageMemoryBarrierCount = 0,
        .pImageMemoryBarriers = nullptr,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
}

[[nodiscard]] VkBufferMemoryBarrier2 bufferBar(VkBuffer buffer,
                                               VkPipelineStageFlags2 srcStage,
                                               VkAccessFlags2 srcAccess,
                                               VkPipelineStageFlags2 dstStage,
                                               VkAccessFlags2 dstAccess) noexcept {
    return VkBufferMemoryBarrier2{
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = srcStage,
        .srcAccessMask = srcAccess,
        .dstStageMask = dstStage,
        .dstAccessMask = dstAccess,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
}

/// vkCmdUpdateBuffer in <=64 KiB 4-byte-multiple chunks (the per-call cap).
void updateBufferChunked(VkCommandBuffer cmd, VkBuffer buffer, std::span<const std::byte> data) noexcept {
    constexpr VkDeviceSize kChunk = 65532;
    for (VkDeviceSize offset = 0; offset < data.size(); offset += kChunk) {
        const VkDeviceSize bytes = (data.size() - offset) < kChunk ? (data.size() - offset) : kChunk;
        vkCmdUpdateBuffer(cmd, buffer, offset, bytes, data.data() + offset);
    }
}

} // namespace

DeformPass::~DeformPass() noexcept {
    if (m_ctx != nullptr) {
        m_writer.shutdown();
    }
}

std::expected<DeformPass, VkResult> DeformPass::create(const DeviceContext& ctx,
                                                       SceneBase& scene,
                                                       const core::Buffer& restVertices,
                                                       const core::Buffer& sceneVertices,
                                                       const std::filesystem::path& computeSpvPath) {
    if (!ctx.isValid() || scene.deformJobs().empty() || !restVertices.isValid() || !sceneVertices.isValid() ||
        restVertices.size() != sceneVertices.size()) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }
    // A job carrying morph fields needs the delta + staged-weight buffers; a
    // job carrying a skin needs its rows (all validated/built by build()).
    bool anyMorph = false;
    bool anySkin = false;
    for (const DeformJob& job : scene.deformJobs()) {
        anyMorph = anyMorph || job.morphTargetCount > 0;
        anySkin = anySkin || job.jointCount > 0;
    }
    if ((anyMorph && (!scene.morphDeltaBuffer().isValid() || !scene.morphWeightBuffer().isValid())) ||
        (anySkin && !scene.skinIndexBuffer().isValid())) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    // 0/1/2: skin index/weight/matrix (NULL descriptors on morph-only scenes -
    // nullDescriptor binds them safely and the kernel never reads them at
    // jointCount == 0); 3: immutable rest-pose vertices (read source); 4:
    // active scene-global vertices (deformed write target); 5: morph deltas;
    // 6: staged morph weights (re-uploaded each tick).
    std::array<VkDescriptorSetLayoutBinding, 7> bindings{};
    for (std::uint32_t b = 0; b < bindings.size(); ++b) {
        bindings[b] = VkDescriptorSetLayoutBinding{
            .binding = b,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        };
    }
    const VkDescriptorSetLayoutCreateInfo setLayoutInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
        .bindingCount = static_cast<std::uint32_t>(bindings.size()),
        .pBindings = bindings.data(),
    };

    DeformPass pass;
    pass.m_ctx = &ctx;
    VkDescriptorSetLayout setLayout{};
    if (const VkResult result = vkCreateDescriptorSetLayout(ctx.device, &setLayoutInfo, nullptr, &setLayout);
        result != VK_SUCCESS) {
        return std::unexpected(result);
    }
    pass.m_setLayout = core::UniqueDescriptorSetLayout{ctx.device, setLayout};

    const VkPushConstantRange pushRange{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(DeformPush),
    };
    const VkPipelineLayoutCreateInfo pipelineLayoutInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = pass.m_setLayout.ptr(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushRange,
    };
    VkPipelineLayout pipelineLayout{};
    if (const VkResult result = vkCreatePipelineLayout(ctx.device, &pipelineLayoutInfo, nullptr, &pipelineLayout);
        result != VK_SUCCESS) {
        return std::unexpected(result);
    }
    pass.m_pipelineLayout = core::UniquePipelineLayout{ctx.device, pipelineLayout};

    auto module = core::createShaderModule(ctx.device, computeSpvPath);
    if (!module) {
        return std::unexpected(module.error());
    }
    const VkComputePipelineCreateInfo pipelineInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
        .stage =
            VkPipelineShaderStageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = *module,
                .pName = "main",
                .pSpecializationInfo = nullptr,
            },
        .layout = pass.m_pipelineLayout,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = 0,
    };
    auto pipelineResult = core::PipelineBinaryCache::computePipeline(ctx, pipelineInfo, "harmonia.deform.pipeline");
    vkDestroyShaderModule(ctx.device, *module, nullptr);
    if (!pipelineResult) {
        return std::unexpected(pipelineResult.error());
    }
    pass.m_pipeline = core::UniquePipeline{ctx.device, *pipelineResult};

    // One descriptor set, written once: build-time buffers are never replaced.
    // Raw-handle writes: VK_NULL_HANDLE becomes a null descriptor (the
    // morph-only skin buffers / skin-only morph buffers never bind garbage).
    if (!pass.m_writer.init(ctx, pass.m_setLayout.get(), 7, "harmonia.deform.descriptors")) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }
    pass.m_writer.writeStorageBufferHandle(ctx, 0, scene.skinIndexBuffer().handle(), scene.skinIndexBuffer().size());
    pass.m_writer.writeStorageBufferHandle(ctx, 1, scene.skinWeightBuffer().handle(), scene.skinWeightBuffer().size());
    pass.m_writer.writeStorageBufferHandle(ctx, 2, scene.skinMatrixBuffer().handle(), scene.skinMatrixBuffer().size());
    pass.m_writer.writeStorageBufferHandle(ctx, 3, restVertices.handle(), restVertices.size());
    pass.m_writer.writeStorageBufferHandle(ctx, 4, sceneVertices.handle(), sceneVertices.size());
    pass.m_writer.writeStorageBufferHandle(ctx, 5, scene.morphDeltaBuffer().handle(), scene.morphDeltaBuffer().size());
    pass.m_writer.writeStorageBufferHandle(
        ctx, 6, scene.morphWeightBuffer().handle(), scene.morphWeightBuffer().size());

    ctx.setDebugName(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, pass.m_setLayout.get(), "harmonia.deform.setLayout");
    ctx.setDebugName(VK_OBJECT_TYPE_PIPELINE_LAYOUT, pass.m_pipelineLayout.get(), "harmonia.deform.pipelineLayout");
    ctx.setDebugName(VK_OBJECT_TYPE_PIPELINE, pass.m_pipeline.get(), "harmonia.deform.pipeline");
    return pass;
}

VkResult DeformPass::execute(const DeviceContext& ctx,
                             const core::CommandPool& pool,
                             SceneBase& scene,
                             float seconds,
                             const core::Buffer& restVertices,
                             const core::Buffer& sceneVertices,
                             std::span<const std::uint32_t> meshSceneVertexOffsets) {
    if (scene.deformJobs().empty()) {
        return VK_SUCCESS; // static scenes never deform
    }
    if (meshSceneVertexOffsets.size() < scene.meshes().size() || restVertices.size() != sceneVertices.size()) {
        harmonia::core::Logger::error("DeformPass: meshSceneVertexOffsets undersized or vertex buffers diverged");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // 1) per-tick staging (each half is a success no-op when the scene lacks it).
    if (scene.hasSkins()) {
        const std::vector<aether::SkinPose> poses = scene.evaluateSkins(seconds);
        if (const VkResult staged = scene.stageSkinMatrices(poses); staged != VK_SUCCESS) {
            return staged;
        }
    }
    if (scene.hasMorphs()) {
        const std::vector<float> weights = scene.evaluateMorphWeights(seconds);
        if (const VkResult staged = scene.stageMorphWeights(weights); staged != VK_SUCCESS) {
            return staged;
        }
    }

    auto cmdResult = pool.beginOneShot();
    if (!cmdResult) {
        return cmdResult.error();
    }
    const VkCommandBuffer cmd = *cmdResult;

    // 2) upload the staged inputs, then the TRANSFER -> COMPUTE barrier.
    std::vector<VkBufferMemoryBarrier2> stageBarriers;
    const std::span<const std::byte> matrixScratch = scene.skinMatrixScratch();
    if (!matrixScratch.empty()) {
        updateBufferChunked(cmd, scene.skinMatrixBuffer().handle(), matrixScratch);
        stageBarriers.push_back(bufferBar(scene.skinMatrixBuffer().handle(),
                                          VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                          VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                          VK_ACCESS_2_SHADER_READ_BIT));
    }
    const std::span<const std::byte> morphScratch = scene.morphWeightScratch();
    if (!morphScratch.empty()) {
        updateBufferChunked(cmd, scene.morphWeightBuffer().handle(), morphScratch);
        stageBarriers.push_back(bufferBar(scene.morphWeightBuffer().handle(),
                                          VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                          VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                          VK_ACCESS_2_SHADER_READ_BIT));
    }
    if (!stageBarriers.empty()) {
        bufferBarrier(cmd, stageBarriers);
    }

    // 3) one dispatch per job (push constants carry the job's ranges; the
    // descriptor set is bound once - all seven buffers are job-invariant).
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    m_writer.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout);
    constexpr VkDeviceSize kDeltaElements = 16; // pad-w float4 rows
    for (const DeformJob& job : scene.deformJobs()) {
        if (job.meshIndex >= meshSceneVertexOffsets.size() || job.vertexCount == 0 ||
            (job.jointCount == 0 && job.morphTargetCount == 0)) {
            continue;
        }
        const core::Buffer* meshVertices = scene.meshes()[job.meshIndex]->storageVertexBuffer();
        if (meshVertices == nullptr) {
            continue; // loader rejects analytic-geometry deformation; defensive
        }
        const std::uint32_t sceneOffset = meshSceneVertexOffsets[job.meshIndex];
        const VkDeviceSize sceneBytes = static_cast<VkDeviceSize>(sceneOffset + job.vertexCount) * sizeof(GpuVertex);
        const VkDeviceSize deltaEnd = static_cast<VkDeviceSize>(job.morphDeltaOffset) +
                                      static_cast<VkDeviceSize>(job.morphTargetCount - 1) * job.morphDeltaStride +
                                      job.vertexCount; // last target's base + its rows
        if (sceneBytes > sceneVertices.size() || sceneBytes > restVertices.size() ||
            (job.morphTargetCount > 0 && (deltaEnd * kDeltaElements > scene.morphDeltaBuffer().size() ||
                                          static_cast<VkDeviceSize>(job.morphWeightOffset) + job.morphTargetCount >
                                              scene.morphWeightScratch().size() / sizeof(float)))) {
            harmonia::core::Logger::error("DeformPass: job range exceeds its buffer (mesh {})", job.meshIndex);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const DeformPush push{
            .vertexCount = job.vertexCount,
            .sceneVertexOffset = sceneOffset,
            .weightRow = job.weightRow,
            .matrixOffset = job.matrixOffset,
            .jointCount = job.jointCount,
            .morphDeltaOffset = job.morphDeltaOffset,
            .morphDeltaStride = job.morphDeltaStride,
            .morphTargetCount = job.morphTargetCount,
            .morphWeightOffset = job.morphWeightOffset,
        };
        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DeformPush), &push);
        vkCmdDispatch(cmd, (job.vertexCount + 63U) / 64U, 1, 1);
    }

    // 4) compute writes -> transfer reads of the scene-global buffer.
    {
        const std::array toCopy{
            bufferBar(sceneVertices.handle(),
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                      VK_ACCESS_2_TRANSFER_READ_BIT),
        };
        bufferBarrier(cmd, toCopy);
    }

    // 5) copy each deformed range into the mesh-local buffer (the BLAS input),
    // then the read barriers:
    //   mesh-local: TRANSFER write -> AS build (refit) + shader/transfer reads;
    //   scene-global: COMPUTE writes -> this frame's shading reads.
    std::vector<VkBufferMemoryBarrier2> postCopy;
    postCopy.reserve(scene.deformJobs().size() + 1);
    for (const DeformJob& job : scene.deformJobs()) {
        if (job.meshIndex >= meshSceneVertexOffsets.size() || job.vertexCount == 0 ||
            (job.jointCount == 0 && job.morphTargetCount == 0)) {
            continue;
        }
        const core::Buffer* meshVertices = scene.meshes()[job.meshIndex]->storageVertexBuffer();
        if (meshVertices == nullptr) {
            continue;
        }
        const VkBufferCopy region{
            .srcOffset = static_cast<VkDeviceSize>(meshSceneVertexOffsets[job.meshIndex]) * sizeof(GpuVertex),
            .dstOffset = 0,
            .size = static_cast<VkDeviceSize>(job.vertexCount) * sizeof(GpuVertex),
        };
        vkCmdCopyBuffer(cmd, sceneVertices.handle(), meshVertices->handle(), 1, &region);
        postCopy.push_back(bufferBar(meshVertices->handle(),
                                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                     VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                     VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                     VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_SHADER_READ_BIT |
                                         VK_ACCESS_2_TRANSFER_READ_BIT));
    }
    postCopy.push_back(
        bufferBar(sceneVertices.handle(),
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                  VK_ACCESS_2_SHADER_READ_BIT));
    bufferBarrier(cmd, postCopy);

    // 6) submit (fence-waited), then refit the dynamic BLASes over the fresh
    // mesh-local data. The shading reads later this frame see the barrier above.
    if (const VkResult submitted = pool.endOneShot(cmd); submitted != VK_SUCCESS) {
        return submitted;
    }
    return scene.refitDynamicBlases(ctx, pool);
}

} // namespace harmonia::scene
