#ifndef HARMONIA_RENDERER_CAMERA_HPP
#define HARMONIA_RENDERER_CAMERA_HPP

#include <cmath>
#include <cstdint>
#include <slang-math/slang-math.hpp>
#include <utility>

#include "harmonia/GpuTypes.hpp"

namespace harmonia::renderer {

/// Vertical FOV used when a scene carries neither an inline `vertical_field_of_view`
/// nor a resolvable `[camera] reference` preset (staged qualification fixtures ship
/// without presets/). Hoisted by the cross-renderer audit decision (2026-11-19): Theia
/// had `value_or(39.1f)` and Hyperion `value_or(kCornellVfovDeg = 39.1f)` as two
/// private copies - the same class as the ev100/vfov parity defects of 2026-11-19,
/// which no shared-estimator test could see. The value is unchanged; the copy is gone.
inline constexpr float kDefaultVfovDeg = 39.1f;

class Camera {
  public:
    /// Physical camera properties (Unity HDRP / UE5 convention).
    /// EV100 is derived: EV100 = log2(N² × shutterSpeedHz × 100 / iso)
    struct PhysicalCamera {
        float focalLengthMm = 50.0f;           ///< focal length in mm
        float aperture = 8.0f;                 ///< f-stop (N)
        float shutterSpeedHz = 100.0f;         ///< 1/shutterSpeedHz seconds (e.g. 100 → 1/100 s)
        float iso = 100.0f;                    ///< sensor sensitivity
        sm::float2 sensorSizeMm{36.0f, 24.0f}; ///< full-frame 35 mm default

        /// EV100 = log2(N² × t_inv × 100 / ISO), where t_inv = shutterSpeedHz
        [[nodiscard]] float ev100() const noexcept;

        /// Exposure multiplier: 1 / (1.2 × 2^EV100)
        [[nodiscard]] float exposure() const noexcept;

        /// Convenience factory: fixed aperture=1, iso=100, compute shutterSpeed from ev100.
        /// Formula: shutterSpeedHz = pow(2, ev100) with aperture=1, iso=100.
        [[nodiscard]] static PhysicalCamera fromEv100(float ev100) noexcept {
            return PhysicalCamera{50.0f, 1.0f, std::pow(2.0f, ev100), 100.0f, {36.0f, 24.0f}};
        }
    };

    struct Params {
        sm::float3 position{0.0f, 0.0f, 10.0f};
        sm::float3 target{0.0f, 0.0f, 0.0f};
        sm::float3 up{0.0f, 1.0f, 0.0f};
        float vfovDeg{39.1f};
        float aspectRatio{16.0f / 9.0f};
        float nearPlane{0.1f};
        float farPlane{10000.0f};
        float lensRadius{0.0f};
        float focusDist{1.0f};
        PhysicalCamera physical{};
    };

    explicit Camera() noexcept;
    explicit Camera(const Params& params) noexcept;

    /// Compute scene-scale-aware near and far planes from the camera-to-target
    /// distance. Used by both Hyperion and Theia to avoid clipping sub-unit or
    /// very large scenes.
    [[nodiscard]] static std::pair<float, float> nearFarFromDistance(float camDist) noexcept;

    void setAspect(float aspect) noexcept;
    void setPosition(sm::float3 pos) noexcept;
    void setTarget(sm::float3 target) noexcept;
    /// ANI8 camera-from-node: world up of the placed camera (mirrors
    /// setPosition/setTarget for the animated-placement path).
    void setUp(sm::float3 up) noexcept;
    /// ANI8 camera-from-node: scene-scale-aware clip planes recomputed from the
    /// placed camera-to-target distance (nearFarFromDistance).
    void setNearFar(float nearPlane, float farPlane) noexcept;
    void setPhysicalCamera(const PhysicalCamera& physical) noexcept;

    [[nodiscard]] const PhysicalCamera& physicalCamera() const noexcept { return m_params.physical; }
    [[nodiscard]] const Params& params() const noexcept { return m_params; }
    [[nodiscard]] CameraData getCameraData(std::uint32_t frameIndex, std::uint32_t maxDepth) const noexcept;

  private:
    [[nodiscard]] sm::float4x4 viewMatrix() const noexcept;
    [[nodiscard]] sm::float4x4 projectionMatrix() const noexcept;

    Params m_params;
};

} // namespace harmonia::renderer

#endif // HARMONIA_RENDERER_CAMERA_HPP
