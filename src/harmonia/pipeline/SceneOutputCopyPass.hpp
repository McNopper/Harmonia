#pragma once

#include <volk/volk.h>

#include <expected>
#include <filesystem>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/Image.hpp"
#include "harmonia/pipeline/IRenderPass.hpp"

struct PassContext;

namespace harmonia {

/// Configuration for the denoiser/accumulation pass.
/// Defined outside SceneOutputCopyPass to avoid CWG2664 (Clang 22+ C++23 restriction on nested-struct
/// NSDMIs in default arguments / data-member initializers of the enclosing class).
struct SceneOutputCopyPassSettings {
    float strength = 0.45F;      ///< spatial denoiser strength [0, 1]
    uint32_t iterations = 2U;    ///< bilateral passes [1, 8]
    bool useHistory = true;      ///< enable temporal blending in fixed-view mode
    float historyBlend = 0.15F;  ///< temporal blend factor [0, 1]
};

/// Shared denoiser stage operating on scene-referred HDR output before tone mapping.
class SceneOutputCopyPass final : public IRenderPass {
  public:
    /// \deprecated Use harmonia::SceneOutputCopyPassSettings directly.
    using Settings = SceneOutputCopyPassSettings;

    [[nodiscard]] static std::expected<SceneOutputCopyPass, VkResult> create(const DeviceContext& ctx,
                                                                              VkExtent2D extent,
                                                                              const std::filesystem::path& computeSpvPath,
                                                                              Settings settings = {});

    SceneOutputCopyPass() = default;
    SceneOutputCopyPass(const SceneOutputCopyPass&) = delete;
    SceneOutputCopyPass& operator=(const SceneOutputCopyPass&) = delete;
    SceneOutputCopyPass(SceneOutputCopyPass&& other) noexcept;
    SceneOutputCopyPass& operator=(SceneOutputCopyPass&& other) noexcept;
    ~SceneOutputCopyPass() noexcept;

    void record(const PassContext& ctx) noexcept override;
    void onResize(VkExtent2D extent) noexcept override;
    [[nodiscard]] const char* name() const noexcept override { return "SceneDenoiserPass"; }

  private:
    [[nodiscard]] bool createWorkImages(VkExtent2D extent) noexcept;
    void resetHistory(uint64_t resetToken) noexcept;
    void destroy() noexcept;

    const DeviceContext* m_ctx = nullptr;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkSampler m_guideSampler = VK_NULL_HANDLE;

    Image m_historyImage{};
    Image m_workImage{};
    Image m_dummyMotionVectors{};  ///< 1×1 R32G32F fallback bound to binding 5 when ctx.motionVectorView is null
    Settings m_settings;  // Default-initialized (uses NSDMIs from SceneOutputCopyPassSettings)
    VkExtent2D m_extent{};
    uint64_t m_lastResetToken = 0U;
    bool m_firstUse = true;
    bool m_historyFirstUse = true;
};

} // namespace harmonia
