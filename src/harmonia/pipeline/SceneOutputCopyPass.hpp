#pragma once

#include <volk/volk.h>

#include "harmonia/pipeline/IRenderPass.hpp"

struct PassContext;

namespace harmonia {

/// Temporary scene-output pass that copies the renderer HDR output into the
/// shared scene-output image before tone mapping. This is the scaffold for the
/// future shared denoiser stage.
class SceneOutputCopyPass final : public IRenderPass {
  public:
    void record(const PassContext& ctx) noexcept override;
    void onResize(VkExtent2D extent) noexcept override;
    [[nodiscard]] const char* name() const noexcept override { return "SceneOutputCopyPass"; }

  private:
    bool m_firstUse = true;
    VkExtent2D m_extent{};
};

} // namespace harmonia
