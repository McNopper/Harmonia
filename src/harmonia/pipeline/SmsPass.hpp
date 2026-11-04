#ifndef HARMONIA_PIPELINE_SMSPASS_HPP
#define HARMONIA_PIPELINE_SMSPASS_HPP

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/Buffer.hpp"
#include "harmonia/core/VulkanHandle.hpp"

namespace harmonia::pipeline {

struct SmsQueueCompletion {
    enum Status : std::uint32_t { Pending = 0, Ready = 1, Failed = 2 };
    std::uint32_t status = Pending;
    std::uint32_t error = 0;
};
static_assert(sizeof(SmsQueueCompletion) == 8);

/// One logical frame's resumable SMS work. Caller retires submissions before
/// reading completion/reusing/destroying resources. No App dependency: renderer
/// adapters implement IFrameCompletion. Host never reads counts for dispatch.
class SmsPass {
  public:
    inline static constexpr VkDeviceSize kWorkRecordSize = 256;
    inline static constexpr VkDeviceSize kQueueHeaderSize = 64;
    inline static constexpr std::uint32_t kPushSize = 64;
    // Slang emits 32-bit OpPtrAccessChain elements (signed in SPIR-V). This is
    // a resource-slot limit, not a reciprocal-trial limit.
    inline static constexpr std::uint32_t kMaxCapacity = 0x7FFFFFFFU;

    /// External layout must outlive the pass and provide a compute push range at
    /// offset zero of at least kPushSize bytes. Step shader uses SmsQueuePush ABI;
    /// scene descriptor buffers, if used, are bound by the caller before batches.
    /// Step local size is exactly (64,1,1); all submissions use the same queue.
    /// pushStages must match the layout's overlapping range (ALL for scene layouts).
    [[nodiscard]] static std::expected<SmsPass, VkResult>
    create(const DeviceContext& ctx,
           std::uint32_t capacity,
           VkPipelineLayout layout,
           const std::filesystem::path& controlSpv,
           const std::filesystem::path& stepSpv,
           const std::filesystem::path& validateSpv,
           std::uint32_t trialBudget = 16,
           VkShaderStageFlags pushStages = VK_SHADER_STAGE_COMPUTE_BIT,
           const std::filesystem::path& priorSpv = {},
           VkDeviceAddress userData = 0,
           const std::filesystem::path& temporalSpv = {});

    [[nodiscard]] VkResult recordBegin(VkCommandBuffer cmd) noexcept;
    [[nodiscard]] VkResult recordBatch(VkCommandBuffer cmd) noexcept;

    /// Only status/error are copied. Caller must have waited for the last batch.
    [[nodiscard]] std::expected<SmsQueueCompletion, VkResult> completion() const noexcept;
    [[nodiscard]] const core::Buffer& workBuffer() const noexcept { return m_work; }
    [[nodiscard]] std::uint32_t capacity() const noexcept { return m_capacity; }

  private:
    void control(VkCommandBuffer cmd, std::uint32_t operation) const noexcept;
    void push(VkCommandBuffer cmd, std::uint32_t operation) const noexcept;

    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkShaderStageFlags m_pushStages = VK_SHADER_STAGE_COMPUTE_BIT;
    core::UniquePipeline m_control;
    core::UniquePipeline m_step;
    core::UniquePipeline m_validate;
    core::UniquePipeline m_prior;
    core::UniquePipeline m_temporal;
    VkDeviceAddress m_userData = 0;
    core::Buffer m_work;
    std::array<core::Buffer, 2> m_indices;
    std::array<core::Buffer, 2> m_headers;
    std::uint32_t m_capacity = 0;
    std::uint32_t m_trialBudget = 0;
    std::uint32_t m_input = 0;
    bool m_started = false;
    bool m_priorRecorded = false;
};

} // namespace harmonia::pipeline

#endif // HARMONIA_PIPELINE_SMSPASS_HPP
