#ifndef HARMONIA_SCENE_LIGHT_HPP
#define HARMONIA_SCENE_LIGHT_HPP

#include <bit>
#include <cstdint>
#include <memory>
#include <numbers>
#include <slang-math/slang-math.hpp>
#include <string>

#include "harmonia/GpuTypes.hpp"

namespace harmonia::scene {

/// Photometric unit for light intensity input.
enum class IntensityUnit : std::uint32_t {
    Nit = 0,     ///< cd/m²  — luminance       (RectLight, SkyLight)
    Candela = 1, ///< cd     — radiant intensity (PointLight, SpotLight)
    Lumen = 2,   ///< lm     — luminous flux     (PointLight, SpotLight isotropic)
    Lux = 3,     ///< lx     — illuminance       (DirectionalLight)
};

/// Abstract base for all scene lights.
///
/// Colors are in linear Rec.2020.
/// Subclasses store intensity in the photometric unit natural to their type,
/// and toGpu() passes it through unchanged: scene-linear radiance is
/// photometric everywhere in Harmonia. The environment's
/// `environment_unit_nits` and a material's `emission_luminance` reach the
/// shaders in cd/m² as well, so an analytic light and an emissive or
/// environment source with the same photometric value shade alike. A shader
/// reading `GpuLight::intensity` therefore gets:
///   Rect          luminance         [cd/m²]   (radiance of the emitting face)
///   Point / Spot  luminous intensity [cd]     (falls off with 1/d²: cd/m² = lux)
///   Directional   illuminance       [lux]
class Light {
  public:
    virtual ~Light() = default;

    virtual LightType lightType() const noexcept = 0;
    virtual GpuLight toGpu() const noexcept = 0;

    std::string name;
    sm::float3 color{1.0f, 1.0f, 1.0f}; ///< linear Rec.2020 chromaticity, default white
};

/// Rectangular area light (two-sided emitter).
/// Intensity unit: cd/m² (nits / luminance).
class RectLight final : public Light {
  public:
    sm::float3 position{0.0f, 0.0f, 0.0f};
    sm::float3 direction{0.0f, -1.0f, 0.0f}; ///< emission normal (downward default)
    /// Orientation of the rectangle in its plane: its local X axis, the
    /// direction `halfWidth` spans (`halfHeight` spans the perpendicular
    /// in-plane axis). Any in-plane vector works; it is normalized against
    /// `direction`. Zero (the default) keeps the canonical in-plane frame the
    /// rect sampling shaders build from the normal alone.
    sm::float3 tangent{0.0f, 0.0f, 0.0f};
    float halfWidth{0.5f};   ///< half-extent in local X
    float halfHeight{0.5f};  ///< half-extent in local Y
    float luminance{100.0f}; ///< cd/m²

    LightType lightType() const noexcept override { return LightType::Rect; }
    GpuLight toGpu() const noexcept override;
};

/// Omnidirectional point light.
/// Intensity unit: cd (default) or lm (isotropic).
class PointLight final : public Light {
  public:
    sm::float3 position{0.0f, 0.0f, 0.0f};
    float intensity{1000.0f}; ///< in `unit`
    IntensityUnit unit{IntensityUnit::Candela};
    float range{0.0f}; ///< 0 = physically correct falloff

    LightType lightType() const noexcept override { return LightType::Point; }
    GpuLight toGpu() const noexcept override;
};

/// Spot light (cone with inner/outer penumbra).
/// Intensity unit: cd (default) or lm.
class SpotLight final : public Light {
  public:
    sm::float3 position{0.0f, 0.0f, 0.0f};
    sm::float3 direction{0.0f, -1.0f, 0.0f};
    float intensity{1000.0f}; ///< in `unit`
    IntensityUnit unit{IntensityUnit::Candela};
    float innerAngleDeg{20.0f};
    float outerAngleDeg{30.0f};
    float range{0.0f}; ///< 0 = no range cutoff

    LightType lightType() const noexcept override { return LightType::Spot; }
    GpuLight toGpu() const noexcept override;
};

/// Infinitely distant directional light (sun/moon).
/// Intensity unit: lux (illuminance).
class DirectionalLight final : public Light {
  public:
    sm::float3 direction{0.0f, -1.0f, 0.0f}; ///< direction light travels (toward scene)
    float illuminance{100'000.0f};           ///< lux; clear-sky sun ≈ 100 000 lx

    LightType lightType() const noexcept override { return LightType::Directional; }
    GpuLight toGpu() const noexcept override;
};

/// core::Image-based sky / ambient light.
/// Intensity unit: cd/m² (average sky luminance).
/// The actual IBL texture is managed separately; this carries the tint and scale.
class SkyLight final : public Light {
  public:
    float luminance{1000.0f}; ///< cd/m² — scales env map radiance

    LightType lightType() const noexcept override { return LightType::Sky; }
    GpuLight toGpu() const noexcept override;
};

} // namespace harmonia::scene

#endif // HARMONIA_SCENE_LIGHT_HPP
