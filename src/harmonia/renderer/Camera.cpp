#include "harmonia/renderer/Camera.hpp"

#include <slang-math/slang-math.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

float Camera::PhysicalCamera::ev100() const noexcept {
    // EV100 = log2(N² × t_inv × 100 / ISO)
    return std::log2f((aperture * aperture) * shutterSpeedHz * 100.0f / iso);
}

float Camera::PhysicalCamera::exposure() const noexcept {
    // Standard photographic exposure: 1 / (1.2 × 2^EV100)
    return 1.0f / (1.2f * std::exp2f(ev100()));
}

Camera::Camera() noexcept : Camera(Params{}) {}

std::pair<float, float> Camera::nearFarFromDistance(float camDist) noexcept {
    return {std::max(0.001f, camDist * 0.01f), std::max(10.0f, camDist * 1000.0f)};
}

Camera::Camera(const Params& params) noexcept : m_params(params) {
    if (m_params.focusDist <= 0.0f) {
        m_params.focusDist = sm::length(m_params.target - m_params.position);
    }
}

void Camera::setAspect(float aspect) noexcept {
    m_params.aspectRatio = std::max(aspect, 0.001f);
}

void Camera::setPosition(sm::float3 pos) noexcept {
    m_params.position = pos;
    if (m_params.focusDist <= 0.0f) {
        m_params.focusDist = sm::length(m_params.target - m_params.position);
    }
}

void Camera::setTarget(sm::float3 target) noexcept {
    m_params.target = target;
    if (m_params.focusDist <= 0.0f) {
        m_params.focusDist = sm::length(m_params.target - m_params.position);
    }
}

void Camera::setPhysicalCamera(PhysicalCamera physical) noexcept {
    m_params.physical = physical;
}

CameraData Camera::getCameraData(uint32_t frameIndex, uint32_t maxDepth) const noexcept {
    const sm::float4x4 view = viewMatrix();
    const sm::float4x4 proj = projectionMatrix();

    return CameraData{
        .invView = sm::inverse(view),
        .invProj = sm::inverse(proj),
        .position = sm::float4(m_params.position, 1.0f),
        .lensRadius = std::max(m_params.lensRadius, 0.0f),
        .focusDistance =
            m_params.focusDist > 0.0f ? m_params.focusDist : sm::length(m_params.target - m_params.position),
        .frameIndex = frameIndex,
        .maxDepth = maxDepth,
        .exposure = m_params.physical.exposure(),
        ._padCam = {},
    };
}

sm::float4x4 Camera::viewMatrix() const noexcept {
    return sm::lookAtRH(m_params.position, m_params.target, m_params.up);
}

sm::float4x4 Camera::projectionMatrix() const noexcept {
    return sm::perspectiveRH_ZO(sm::radians(m_params.vfovDeg),
                                std::max(m_params.aspectRatio, 0.001f),
                                std::max(m_params.nearPlane, 0.001f),
                                std::max(m_params.farPlane, m_params.nearPlane + 0.001f));
}
