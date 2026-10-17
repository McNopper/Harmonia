#ifndef HARMONIA_SCENE_RT_SPHERE_HPP
#define HARMONIA_SCENE_RT_SPHERE_HPP

#include <volk/volk.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/Buffer.hpp"
#include "harmonia/core/CommandPool.hpp"
#include "harmonia/rt/AccelerationStructure.hpp"
#include "harmonia/scene/Geometry.hpp"

namespace harmonia::scene_rt {

/// An analytic sphere with its ray-tracing state: the AABB BLAS build and the
/// TLAS instance emission. The pure description lives in harmonia::scene
/// (radius + Transform placement).
class Sphere final : public harmonia::scene::Geometry {
  public:
    /// Analytic sphere of @p radius centred at the object-space origin. Placement
    /// is the instance transform's job (translation/scale via the TLAS).
    [[nodiscard]] static std::expected<std::unique_ptr<Sphere>, VkResult>
    create(const DeviceContext& ctx, const core::CommandPool& pool, float radius, std::string_view debugName = "");

    VkResult buildBlas(const DeviceContext& ctx, const core::CommandPool& pool) override;
    [[nodiscard]] VkAccelerationStructureInstanceKHR
    makeInstance(std::uint32_t instanceCustomIndex,
                 const harmonia::scene::Transform& transform) const noexcept override;
    [[nodiscard]] harmonia::scene::Aabb objectAabb() const noexcept override;

    [[nodiscard]] float radius() const noexcept;

  private:
    float m_radius = 0.0f;
    core::Buffer m_aabbBuffer{};
    rt::AccelerationStructure m_accelerationStructure{};
    std::string m_debugName{};
};

} // namespace harmonia::scene_rt

#endif // HARMONIA_SCENE_RT_SPHERE_HPP
