#ifndef HARMONIA_PIPELINE_ACCUMULATIONPASS_HPP
#define HARMONIA_PIPELINE_ACCUMULATIONPASS_HPP

#include <volk/volk.h>

#include <cstdint>
#include <expected>
#include <filesystem>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/Image.hpp"
#include "harmonia/core/VulkanHandle.hpp"
#include "harmonia/pipeline/IRenderPass.hpp"

namespace harmonia {

struct PassContext;

/// Fixed-view progressive accumulation pass.
///
/// Reads the renderer's current scene-referred HDR frame, updates an internal
/// running-average history image, then writes the averaged result back to the
/// HDR image for downstream stages.
class AccumulationPass final : public IRenderPass {
  public:
    [[nodiscard]] static std::expected<AccumulationPass, VkResult>
    create(const DeviceContext& ctx, VkExtent2D extent, const std::filesystem::path& computeSpvPath);

    AccumulationPass() = default;
    AccumulationPass(const AccumulationPass&) = delete;
    AccumulationPass& operator=(const AccumulationPass&) = delete;
    AccumulationPass(AccumulationPass&&) noexcept = default;
    AccumulationPass& operator=(AccumulationPass&&) noexcept = default;
    ~AccumulationPass() noexcept override = default;

    void record(const PassContext& ctx) noexcept override;
    void onResize(VkExtent2D extent) noexcept override;
    [[nodiscard]] const char* name() const noexcept override { return "AccumulationPass"; }

  private:
    [[nodiscard]] bool createHistoryImage(VkExtent2D extent) noexcept;
    void resetHistory(std::uint64_t resetToken) noexcept;

    const DeviceContext* m_ctx = nullptr;
    UniquePipeline m_pipeline;
    UniquePipelineLayout m_pipelineLayout;
    UniqueDescriptorSetLayout m_setLayout;

    Image m_historyImage{};
    VkExtent2D m_extent{};
    std::uint32_t m_sampleCount = 0;
    std::uint64_t m_lastResetToken = 0;
    bool m_historyFirstUse = true;
};

} // namespace harmonia
#endif // HARMONIA_PIPELINE_ACCUMULATIONPASS_HPP
