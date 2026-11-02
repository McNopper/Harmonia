#include "harmonia/pipeline/SmsPass.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "harmonia/core/ShaderModule.hpp"

namespace harmonia::pipeline {
namespace {
struct SmsQueuePush {
    VkDeviceAddress work, inputIndices, outputIndices, inputHeader, outputHeader;
    std::uint32_t capacity, operation, trialBudget, pad;
};
static_assert(sizeof(SmsQueuePush) == SmsPass::kPushSize);

std::expected<core::UniquePipeline, VkResult>
createPipeline(const DeviceContext& ctx, VkPipelineLayout layout, const std::filesystem::path& path) {
    auto rawModule = core::createShaderModule(ctx.device, path);
    if (!rawModule)
        return std::unexpected(rawModule.error());
    const core::UniqueShaderModule module{ctx.device, *rawModule};
    const VkComputePipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                           .pNext = nullptr,
                                           .flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
                                           .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                     .pNext = nullptr,
                                                     .flags = 0,
                                                     .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                     .module = module,
                                                     .pName = "main",
                                                     .pSpecializationInfo = nullptr},
                                           .layout = layout,
                                           .basePipelineHandle = VK_NULL_HANDLE,
                                           .basePipelineIndex = -1};
    VkPipeline pipeline{};
    const auto result = vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    if (result != VK_SUCCESS)
        return std::unexpected(result);
    return core::UniquePipeline{ctx.device, pipeline};
}

void queueBarrier(VkCommandBuffer cmd) {
    const VkMemoryBarrier2 memory{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                  .pNext = nullptr,
                                  .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                  .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
                                  .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                                  VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_HOST_BIT,
                                  .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
                                                   VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_HOST_READ_BIT};
    const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                      .pNext = nullptr,
                                      .dependencyFlags = 0,
                                      .memoryBarrierCount = 1,
                                      .pMemoryBarriers = &memory,
                                      .bufferMemoryBarrierCount = 0,
                                      .pBufferMemoryBarriers = nullptr,
                                      .imageMemoryBarrierCount = 0,
                                      .pImageMemoryBarriers = nullptr};
    vkCmdPipelineBarrier2(cmd, &dependency);
}
} // namespace

std::expected<SmsPass, VkResult> SmsPass::create(const DeviceContext& ctx,
                                                 std::uint32_t capacity,
                                                 VkPipelineLayout layout,
                                                 const std::filesystem::path& controlSpv,
                                                 const std::filesystem::path& stepSpv,
                                                 std::uint32_t trialBudget) {
    if (!ctx.isValid() || layout == VK_NULL_HANDLE || trialBudget == 0U || capacity > kMaxCapacity)
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    SmsPass pass;
    pass.m_layout = layout;
    pass.m_capacity = capacity;
    pass.m_trialBudget = trialBudget;
    const auto slots = std::max<VkDeviceSize>(capacity, 1);
    constexpr auto storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    auto work = core::Buffer::create(ctx,
                                     slots * kWorkRecordSize,
                                     storage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                     VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                     "sms.work");
    if (!work)
        return std::unexpected(work.error());
    pass.m_work = std::move(*work);
    for (std::size_t i = 0; i < 2; ++i) {
        auto indices = core::Buffer::create(
            ctx, slots * sizeof(std::uint32_t), storage, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, "sms.active");
        if (!indices)
            return std::unexpected(indices.error());
        pass.m_indices[i] = std::move(*indices);
        auto header = core::Buffer::create(ctx,
                                           kQueueHeaderSize,
                                           storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                           VMA_MEMORY_USAGE_GPU_TO_CPU,
                                           "sms.header");
        if (!header)
            return std::unexpected(header.error());
        pass.m_headers[i] = std::move(*header);
    }
    auto control = createPipeline(ctx, layout, controlSpv);
    if (!control)
        return std::unexpected(control.error());
    pass.m_control = std::move(*control);
    auto step = createPipeline(ctx, layout, stepSpv);
    if (!step)
        return std::unexpected(step.error());
    pass.m_step = std::move(*step);
    return pass;
}

void SmsPass::push(VkCommandBuffer cmd, std::uint32_t operation) const noexcept {
    const SmsQueuePush pc{m_work.deviceAddress(),
                          m_indices[m_input].deviceAddress(),
                          m_indices[1U - m_input].deviceAddress(),
                          m_headers[m_input].deviceAddress(),
                          m_headers[1U - m_input].deviceAddress(),
                          m_capacity,
                          operation,
                          m_trialBudget,
                          0};
    vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
}

void SmsPass::control(VkCommandBuffer cmd, std::uint32_t operation) const noexcept {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_control);
    push(cmd, operation);
    vkCmdDispatch(cmd, 1, 1, 1);
    queueBarrier(cmd);
}

VkResult SmsPass::recordBegin(VkCommandBuffer cmd) noexcept {
    if (cmd == VK_NULL_HANDLE || m_control == VK_NULL_HANDLE)
        return VK_ERROR_INITIALIZATION_FAILED;
    m_input = 0;
    queueBarrier(cmd);
    control(cmd, 0);
    m_started = true;
    return VK_SUCCESS;
}

VkResult SmsPass::recordBatch(VkCommandBuffer cmd) noexcept {
    if (!m_started || cmd == VK_NULL_HANDLE || m_step == VK_NULL_HANDLE)
        return VK_ERROR_INITIALIZATION_FAILED;
    queueBarrier(cmd);
    control(cmd, 1);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_step);
    push(cmd, 0);
    vkCmdDispatchIndirect(cmd, m_headers[m_input].handle(), 0);
    queueBarrier(cmd);
    control(cmd, 2);
    m_input = 1U - m_input;
    return VK_SUCCESS;
}

std::expected<SmsQueueCompletion, VkResult> SmsPass::completion() const noexcept {
    if (!m_started)
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    SmsQueueCompletion result;
    if (const auto status = m_headers[m_input].readData(&result, sizeof(result), 5U * sizeof(std::uint32_t));
        status != VK_SUCCESS)
        return std::unexpected(status);
    if (result.status > SmsQueueCompletion::Failed ||
        (result.status == SmsQueueCompletion::Failed) != (result.error != 0U))
        return std::unexpected(VK_ERROR_UNKNOWN);
    return result;
}

} // namespace harmonia::pipeline
