#include "harmonia/scene/Light.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace harmonia::scene {

namespace {

/// Pack a LightType enum value into the float slot of GpuLight.
/// (Matches the asuint() read in the shader.)
inline float packType(LightType t) noexcept {
    const std::uint32_t u = static_cast<std::uint32_t>(t);
    return std::bit_cast<float>(u);
}

/// Convert luminous flux [lm] to luminous intensity [cd] for an isotropic source.
inline float lumenToCandela(float lm) noexcept {
    return lm / (4.0f * sm::pi<float>());
}

/// The in-plane frame of a rect light that its roll is measured from. It is
/// the frame the rect sampling shaders build from the normal alone, so a roll
/// of 0 is exactly that frame and lights that carry no orientation (zero
/// tangent) shade as before. The definition is shared with those shaders and
/// must not drift: `shadeRectLight` in Theia's forward shader mirrors it.
void canonicalRectFrame(const sm::float3& normal, sm::float3& tangent, sm::float3& bitangent) noexcept {
    const sm::float3 up = std::abs(normal.y) < 0.99f ? sm::float3{0.0f, 1.0f, 0.0f} : sm::float3{1.0f, 0.0f, 0.0f};
    tangent = sm::normalize(sm::cross(up, normal));
    bitangent = sm::cross(normal, tangent);
}

/// Roll of @p tangent about @p normal, in radians, from the canonical frame.
/// A tangent with no length carries no orientation and gives 0.
float rectRoll(const sm::float3& normal, const sm::float3& tangent) noexcept {
    const float length = sm::length(tangent);
    if (!(length > 0.0f)) {
        return 0.0f;
    }
    sm::float3 canonicalTangent{};
    sm::float3 canonicalBitangent{};
    canonicalRectFrame(normal, canonicalTangent, canonicalBitangent);
    const sm::float3 direction = tangent / length;
    return std::atan2(sm::dot(direction, canonicalBitangent), sm::dot(direction, canonicalTangent));
}

} // namespace

// ---------------------------------------------------------------------------
// RectLight
// ---------------------------------------------------------------------------
// The descriptor carries the photometric luminance as it is. Its in-plane
// orientation travels as a roll about the normal, in the `cosInner` slot,
// which a rect light does not use: halfWidth spans the rolled axis and
// halfHeight the perpendicular one, so the rectangle keeps its normal and its
// area under any roll.
GpuLight RectLight::toGpu() const noexcept {
    GpuLight g{};
    g.position = position;
    g.type = packType(LightType::Rect);
    g.direction = sm::normalize(direction);
    g.range = 0.0f;
    g.color = color;
    g.intensity = luminance;
    g.halfWidth = halfWidth;
    g.halfHeight = halfHeight;
    g.cosInner = rectRoll(g.direction, tangent);
    g.cosOuter = 0.0f;
    return g;
}

// ---------------------------------------------------------------------------
// PointLight
// ---------------------------------------------------------------------------
GpuLight PointLight::toGpu() const noexcept {
    float cd = intensity;
    if (unit == IntensityUnit::Lumen) {
        cd = lumenToCandela(intensity);
    }

    GpuLight g{};
    g.position = position;
    g.type = packType(LightType::Point);
    g.direction = sm::float3{0.0f, -1.0f, 0.0f}; // unused for point
    g.range = range;
    g.color = color;
    g.intensity = cd;
    g.halfWidth = 0.0f;
    g.halfHeight = 0.0f;
    g.cosInner = 0.0f;
    g.cosOuter = 0.0f;
    return g;
}

// ---------------------------------------------------------------------------
// SpotLight
// ---------------------------------------------------------------------------
GpuLight SpotLight::toGpu() const noexcept {
    float cd = intensity;
    if (unit == IntensityUnit::Lumen) {
        cd = lumenToCandela(intensity);
    }

    GpuLight g{};
    g.position = position;
    g.type = packType(LightType::Spot);
    g.direction = sm::normalize(direction);
    g.range = range;
    g.color = color;
    g.intensity = cd;
    g.halfWidth = 0.0f;
    g.halfHeight = 0.0f;
    g.cosInner = std::cos(sm::radians(innerAngleDeg));
    g.cosOuter = std::cos(sm::radians(outerAngleDeg));
    return g;
}

// ---------------------------------------------------------------------------
// DirectionalLight
// ---------------------------------------------------------------------------
// The shader contribution is color * intensity * BSDF * visibility, with the
// cosine term in the BSDF evaluation: the photometric illuminance in lux.
GpuLight DirectionalLight::toGpu() const noexcept {
    GpuLight g{};
    g.position = sm::float3{0.0f, 0.0f, 0.0f}; // unused
    g.type = packType(LightType::Directional);
    g.direction = sm::normalize(direction);
    g.range = 0.0f;
    g.color = color;
    g.intensity = illuminance;
    g.halfWidth = 0.0f;
    g.halfHeight = 0.0f;
    g.cosInner = 0.0f;
    g.cosOuter = 0.0f;
    return g;
}

// ---------------------------------------------------------------------------
// SkyLight
// ---------------------------------------------------------------------------
// The sky's photometric luminance scale, in cd/m².
GpuLight SkyLight::toGpu() const noexcept {
    GpuLight g{};
    g.position = sm::float3{0.0f, 0.0f, 0.0f};
    g.type = packType(LightType::Sky);
    g.direction = sm::float3{0.0f, 1.0f, 0.0f};
    g.range = 0.0f;
    g.color = color;
    g.intensity = luminance;
    g.halfWidth = 0.0f;
    g.halfHeight = 0.0f;
    g.cosInner = 0.0f;
    g.cosOuter = 0.0f;
    return g;
}

} // namespace harmonia::scene
