#ifndef HARMONIA_RENDERER_PIPELINE_HPP
#define HARMONIA_RENDERER_PIPELINE_HPP

#include <volk/volk.h>

#include <cstdint>
#include <expected>
#include <filesystem>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/VulkanHandle.hpp"

namespace harmonia {

class Descriptors;

class Pipeline {
  public:
    /// RT stage modules. `shadowAnyHit` must expose BOTH `triangleShadowAnyHit`
    /// and `sphereShadowAnyHit`; it is the only stage in the shadow hit groups
    /// and resolves OpenPBR `geometry_opacity` per crossing.
    struct ShaderPaths {
        std::filesystem::path raygen;
        std::filesystem::path closesthitTriangle;
        std::filesystem::path closesthitSphere;
        std::filesystem::path intersection;
        std::filesystem::path miss;
        std::filesystem::path shadowMiss;
        std::filesystem::path shadowAnyHit;
    };

    /// Hit-group record layout of the SBT this pipeline expects. Selected as
    /// `instanceShaderBindingTableRecordOffset + RayContributionToHitGroupIndex`:
    /// triangle instances use offset 0, procedural (sphere) instances offset 2;
    /// radiance rays contribute 0, shadow rays 1.
    static constexpr std::uint32_t kHitGroupCount = 4;

    Pipeline() = default;
    ~Pipeline() noexcept = default;

    Pipeline(Pipeline&&) noexcept = default;
    Pipeline& operator=(Pipeline&&) noexcept = default;

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    [[nodiscard]] static std::expected<Pipeline, VkResult> create(const DeviceContext& ctx,
                                                                  const Descriptors& descriptors,
                                                                  const ShaderPaths& paths,
                                                                  std::uint32_t maxRayRecursion = 8);

    [[nodiscard]] VkPipeline rtPipeline() const noexcept { return m_rtPipeline; }

  private:
    harmonia::UniquePipeline m_rtPipeline;
};

} // namespace harmonia

#endif // HARMONIA_RENDERER_PIPELINE_HPP
