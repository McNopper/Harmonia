#ifndef HARMONIA_RENDERER_DESCRIPTORS_HPP
#define HARMONIA_RENDERER_DESCRIPTORS_HPP

#include <volk/volk.h>

#include <expected>
#include <span>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/GpuTypes.hpp"
#include "harmonia/core/DescriptorBufferWriter.hpp"
#include "harmonia/core/VulkanHandle.hpp"
#include "harmonia/scene/Texture.hpp"

namespace harmonia {

class Descriptors {
  public:
    Descriptors() = default;
    ~Descriptors() noexcept = default;

    Descriptors(Descriptors&&) noexcept = default;
    Descriptors& operator=(Descriptors&&) noexcept = default;

    Descriptors(const Descriptors&) = delete;
    Descriptors& operator=(const Descriptors&) = delete;

    [[nodiscard]] static std::expected<Descriptors, VkResult> create(const DeviceContext& ctx);

    /// Populate the path-tracer scene descriptor set (set 1) from raw GPU buffers and
    /// scene textures. Scene-type-agnostic so each renderer can build its own Scene and
    /// still reuse Harmonia's shared descriptor layout. Buffer bindings: 0=instances,
    /// 1=materials, 2=vertices, 3=indices, 5=lights, 7=emissive triangles,
    /// 10=emissive power CDF; binding 4 = bindless texture array.
    VkResult updateSceneSet(const DeviceContext& ctx,
                            VkBuffer instanceBuffer,
                            VkBuffer materialBuffer,
                            VkBuffer vertexBuffer,
                            VkBuffer indexBuffer,
                            VkBuffer lightBuffer,
                            VkBuffer emissiveTriangleBuffer,
                            VkBuffer emissiveCdfBuffer,
                            std::span<const Texture> textures);
    VkResult updateEnvMap(const DeviceContext& ctx, VkImageView view, VkSampler sampler);
    VkResult updateEnvImportance(const DeviceContext& ctx, VkBuffer marginalCdf, VkBuffer conditionalCdf);

    /// MOD1: update set 0 (per-frame AS + images + camera UBO) via descriptor buffer.
    VkResult updateFrameSet(const DeviceContext& ctx,
                            VkAccelerationStructureKHR tlas,
                            VkImageView hdrView,
                            VkBuffer cameraBuffer,
                            VkImageView gNormalView,
                            VkImageView gDepthView);

    /// MOD1: bind both descriptor buffers (set 0 + set 1). Replaces set1() + vkCmdBindDescriptorSets.
    void bindSceneSet(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint) const;

    [[nodiscard]] VkDescriptorSetLayout set0Layout() const noexcept { return m_set0Layout; }
    [[nodiscard]] VkDescriptorSetLayout set1Layout() const noexcept { return m_set1Layout; }
    [[nodiscard]] VkPipelineLayout pipelineLayout() const noexcept { return m_pipelineLayout; }

  private:
    harmonia::UniqueDescriptorSetLayout m_set0Layout;
    harmonia::UniqueDescriptorSetLayout m_set1Layout;
    /// MOD1: descriptor buffer writers replace the pool + sets.
    DescriptorBufferWriter m_frameWriter;  ///< set 0: per-frame AS + images + camera UBO
    DescriptorBufferWriter m_sceneWriter;  ///< set 1: scene buffers + bindless textures
    harmonia::UniquePipelineLayout m_pipelineLayout;
};

} // namespace harmonia

#endif // HARMONIA_RENDERER_DESCRIPTORS_HPP
