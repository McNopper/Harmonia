#ifndef HARMONIA_RENDERER_DESCRIPTORS_HPP
#define HARMONIA_RENDERER_DESCRIPTORS_HPP

#include <volk/volk.h>

#include <expected>
#include <span>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/GpuTypes.hpp"
#include "harmonia/core/VulkanHandle.hpp"
#include "harmonia/scene/Texture.hpp"

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

    [[nodiscard]] VkDescriptorSetLayout set0Layout() const noexcept { return m_set0Layout; }
    [[nodiscard]] VkDescriptorSetLayout set1Layout() const noexcept { return m_set1Layout; }
    [[nodiscard]] VkDescriptorSet set1() const noexcept { return m_set1; }
    [[nodiscard]] VkPipelineLayout pipelineLayout() const noexcept { return m_pipelineLayout; }

  private:
    harmonia::UniqueDescriptorSetLayout m_set0Layout;
    harmonia::UniqueDescriptorSetLayout m_set1Layout;
    harmonia::UniqueDescriptorPool m_pool;
    VkDescriptorSet m_set1{VK_NULL_HANDLE};
    harmonia::UniquePipelineLayout m_pipelineLayout;
};
#endif // HARMONIA_RENDERER_DESCRIPTORS_HPP
