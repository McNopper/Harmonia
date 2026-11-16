#include "harmonia/scene/SkinDeformPass.hpp"

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

/// Must match the `SkinPush` block of skin_deform.comp.slang (std430, 20 B).
struct SkinPush {
    std::uint32_t vertexCount = 0;
    std::uint32_t sceneVertexOffset = 0;
    std::uint32_t weightRow = 0;
    std::uint32_t matrixOffset = 0;
    std::uint32_t jointCount = 0;
};
static_assert(sizeof(SkinPush) == 20);

/// vkCmdPipelineBarrier2 over buffer barriers only (the pass's barriers are
/// all buffer-scope: transfer/compute/as-build/shader on vertex + matrix data).
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

} // namespace

SkinDeformPass::~SkinDeformPass() noexcept {
    if (m_ctx != nullptr) {
        m_writer.shutdown();
    }
}

std::expected<SkinDeformPass, VkResult> SkinDeformPass::create(const DeviceContext& ctx,
                                                               SceneBase& scene,
                                                               const core::Buffer& restVertices,
                                                               const core::Buffer& sceneVertices,
                                                               const std::filesystem::path& computeSpvPath) {
    if (!ctx.isValid() || scene.skinJobs().empty() || !restVertices.isValid() || !sceneVertices.isValid() ||
        restVertices.size() != sceneVertices.size()) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    // 0/1/2: the scene's skin index/weight/matrix buffers; 3: the renderer's
    // immutable rest-pose vertex copy (the kernel's read source); 4: the active
    // scene-global vertex buffer (the kernel writes the deformed pose there).
    const std::array bindings{
        VkDescriptorSetLayoutBinding{
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        },
        VkDescriptorSetLayoutBinding{
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        },
        VkDescriptorSetLayoutBinding{
            .binding = 2,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        },
        VkDescriptorSetLayoutBinding{
            .binding = 3,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        },
        VkDescriptorSetLayoutBinding{
            .binding = 4,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        },
    };
    const VkDescriptorSetLayoutCreateInfo setLayoutInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
        .bindingCount = static_cast<std::uint32_t>(bindings.size()),
        .pBindings = bindings.data(),
    };

    SkinDeformPass pass;
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
        .size = sizeof(SkinPush),
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
    auto pipelineResult = core::PipelineBinaryCache::computePipeline(ctx, pipelineInfo, "harmonia.skinDeform.pipeline");
    vkDestroyShaderModule(ctx.device, *module, nullptr);
    if (!pipelineResult) {
        return std::unexpected(pipelineResult.error());
    }
    pass.m_pipeline = core::UniquePipeline{ctx.device, *pipelineResult};

    // One descriptor set, written once: the skin buffers + the scene-global
    // vertex buffer are created by build() and never replaced.
    if (!pass.m_writer.init(ctx, pass.m_setLayout.get(), 5, "harmonia.skinDeform.descriptors")) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }
    pass.m_writer.writeStorageBufferHandle(ctx, 0, scene.skinIndexBuffer());
    pass.m_writer.writeStorageBufferHandle(ctx, 1, scene.skinWeightBuffer());
    pass.m_writer.writeStorageBufferHandle(ctx, 2, scene.skinMatrixBuffer());
    pass.m_writer.writeStorageBufferHandle(ctx, 3, restVertices);
    pass.m_writer.writeStorageBufferHandle(ctx, 4, sceneVertices);

    ctx.setDebugName(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, pass.m_setLayout.get(), "harmonia.skinDeform.setLayout");
    ctx.setDebugName(VK_OBJECT_TYPE_PIPELINE_LAYOUT, pass.m_pipelineLayout.get(), "harmonia.skinDeform.pipelineLayout");
    ctx.setDebugName(VK_OBJECT_TYPE_PIPELINE, pass.m_pipeline.get(), "harmonia.skinDeform.pipeline");
    return pass;
}

VkResult SkinDeformPass::execute(const DeviceContext& ctx,
                                 const core::CommandPool& pool,
                                 SceneBase& scene,
                                 float seconds,
                                 const core::Buffer& restVertices,
                                 const core::Buffer& sceneVertices,
                                 std::span<const std::uint32_t> meshSceneVertexOffsets) {
    if (!scene.hasSkins() || scene.skinJobs().empty()) {
        return VK_SUCCESS; // static/skin-free scenes never deform
    }
    if (meshSceneVertexOffsets.size() < scene.meshes().size() || restVertices.size() != sceneVertices.size()) {
        harmonia::core::Logger::error("SkinDeformPass: meshSceneVertexOffsets undersized");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const std::vector<aether::SkinPose> poses = scene.evaluateSkins(seconds);
    if (const VkResult staged = scene.stageSkinMatrices(poses); staged != VK_SUCCESS) {
        return staged;
    }

    auto cmdResult = pool.beginOneShot();
    if (!cmdResult) {
        return cmdResult.error();
    }
    const VkCommandBuffer cmd = *cmdResult;

    // 1) matrix upload: vkCmdUpdateBuffer caps a call at 64 KiB (and needs a
    // 4-byte multiple) - chunked; the buffer carries TRANSFER_DST from create.
    const std::span<const std::byte> scratch = scene.skinMatrixScratch();
    constexpr VkDeviceSize kChunk = 65532; // <= 65536, multiple of 4
    for (VkDeviceSize offset = 0; offset < scratch.size(); offset += kChunk) {
        const VkDeviceSize bytes = (scratch.size() - offset) < kChunk ? (scratch.size() - offset) : kChunk;
        vkCmdUpdateBuffer(cmd, scene.skinMatrixBuffer().handle(), offset, bytes, scratch.data() + offset);
    }
    if (!scratch.empty()) {
        const std::array mtxBarrier{
            bufferBar(scene.skinMatrixBuffer().handle(),
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_READ_BIT),
        };
        bufferBarrier(cmd, mtxBarrier);
    }

    // 2) one dispatch per job (push constants carry the job's ranges; the
    // descriptor set is bound once - all four buffers are job-invariant).
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    m_writer.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout);
    for (const SkinJob& job : scene.skinJobs()) {
        if (job.meshIndex >= meshSceneVertexOffsets.size() || job.vertexCount == 0) {
            continue;
        }
        const core::Buffer* meshVertices = scene.meshes()[job.meshIndex]->storageVertexBuffer();
        if (meshVertices == nullptr) {
            continue; // loader rejects analytic-geometry skins; defensive
        }
        const std::uint32_t sceneOffset = meshSceneVertexOffsets[job.meshIndex];
        const VkDeviceSize sceneBytes = static_cast<VkDeviceSize>(sceneOffset + job.vertexCount) * sizeof(GpuVertex);
        if (sceneBytes > sceneVertices.size() || sceneBytes > restVertices.size()) {
            harmonia::core::Logger::error("SkinDeformPass: job range exceeds the scene vertex buffer");
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const SkinPush push{
            .vertexCount = job.vertexCount,
            .sceneVertexOffset = sceneOffset,
            .weightRow = job.weightRow,
            .matrixOffset = job.matrixOffset,
            .jointCount = job.jointCount,
        };
        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SkinPush), &push);
        vkCmdDispatch(cmd, (job.vertexCount + 63U) / 64U, 1, 1);
    }

    // 3) compute writes -> transfer reads of the scene-global buffer.
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

    // 4) copy each deformed range into the mesh-local buffer (the BLAS input),
    // then the two read barriers:
    //   mesh-local: TRANSFER write -> AS build (refit) + shader reads;
    //   scene-global: COMPUTE writes -> this frame's shading reads.
    std::vector<VkBufferMemoryBarrier2> postCopy;
    postCopy.reserve(scene.skinJobs().size() + 1);
    for (const SkinJob& job : scene.skinJobs()) {
        if (job.meshIndex >= meshSceneVertexOffsets.size() || job.vertexCount == 0) {
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

    // 5) submit (fence-waited), then refit the dynamic BLASes over the fresh
    // mesh-local data. The shading reads later this frame see the barrier above.
    if (const VkResult submitted = pool.endOneShot(cmd); submitted != VK_SUCCESS) {
        return submitted;
    }
    return scene.refitDynamicBlases(ctx, pool);
}

} // namespace harmonia::scene
