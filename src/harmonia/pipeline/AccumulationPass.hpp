#pragma once

#include <volk/volk.h>

#include <expected>
#include <filesystem>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/Image.hpp"
#include "harmonia/pipeline/IRenderPass.hpp"

struct PassContext;

namespace harmonia {

/// Fixed-view progressive accumulation pass.
///
/// Reads the renderer's current scene-referred HDR frame, updates an internal
/// running-average history image, then writes the averaged result back to the
/// HDR image for downstream stages.
class AccumulationPass final : public IRenderPass {
  public:
    [[nodiscard]] static std::expected<AccumulationPass, VkResult> create(const DeviceContext& ctx,
                                                                          VkExtent2D extent,
                                                                          const std::filesystem::path& computeSpvPath);

    AccumulationPass() = default;
    AccumulationPass(const AccumulationPass&) = delete;
    AccumulationPass& operator=(const AccumulationPass&) = delete;
    AccumulationPass(AccumulationPass&& other) noexcept;
    AccumulationPass& operator=(AccumulationPass&& other) noexcept;
    ~AccumulationPass() noexcept;

    void record(const PassContext& ctx) noexcept override;
    void onResize(VkExtent2D extent) noexcept override;
    [[nodiscard]] const char* name() const noexcept override { return "AccumulationPass"; }

  private:
    [[nodiscard]] bool createHistoryImage(VkExtent2D extent) noexcept;
    void resetHistory(uint64_t resetToken) noexcept;
    void destroy() noexcept;

    const DeviceContext* m_ctx = nullptr;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;

    Image m_historyImage{};
    VkExtent2D m_extent{};
    uint32_t m_sampleCount = 0;
    uint64_t m_lastResetToken = 0;
    bool m_historyFirstUse = true;
};

} // namespace harmonia
