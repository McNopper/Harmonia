#include "harmonia/renderer/CameraAssembly.hpp"

#include <algorithm>
#include <cmath>

namespace harmonia::renderer {

std::optional<Camera::Params> CameraAssembly::fromSceneConfig(const harmonia::scene::SceneLoader::SceneConfig& config,
                                                              float aspectRatio) noexcept {
    if (!config.cameraPos || !config.cameraAt) {
        return std::nullopt;
    }

    const sm::float3 position = *config.cameraPos;
    const sm::float3 target = *config.cameraAt;
    const float targetDistance = sm::length(target - position);
    const auto [nearPlane, farPlane] = Camera::nearFarFromDistance(targetDistance);

    Camera::Params params{};
    params.position = position;
    params.target = target;
    params.up = config.cameraUp.value_or(sm::float3{0.0f, 1.0f, 0.0f});
    params.vfovDeg = config.cameraVfov.value_or(kDefaultVfovDeg);
    params.aspectRatio = std::max(aspectRatio, 0.001f);
    params.nearPlane = nearPlane;
    params.farPlane = farPlane;
    params.lensRadius = 0.0f;
    params.focusDist = targetDistance;
    if (config.cameraEv100) {
        params.physical = Camera::PhysicalCamera::fromEv100(*config.cameraEv100);
    }
    return params;
}

Camera::Params CameraAssembly::fromPose(const Camera::Params& base, const aether::CameraPose& pose) noexcept {
    Camera::Params params = base;
    params.position = pose.position;
    params.target = params.position + (pose.rotation * sm::float3{0.0f, 0.0f, -1.0f});
    params.up = pose.rotation * sm::float3{0.0f, 1.0f, 0.0f};
    const float targetDistance = sm::length(params.target - params.position);
    const auto [nearPlane, farPlane] = Camera::nearFarFromDistance(targetDistance);
    params.nearPlane = nearPlane;
    params.farPlane = farPlane;
    if (params.focusDist <= 0.0f) {
        params.focusDist = targetDistance;
    }
    return params;
}

} // namespace harmonia::renderer
