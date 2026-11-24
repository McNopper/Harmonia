#ifndef HARMONIA_RENDERER_CAMERAASSEMBLY_HPP
#define HARMONIA_RENDERER_CAMERAASSEMBLY_HPP

#include <optional>

#include "aether/format/SceneAnimator.hpp"
#include "harmonia/renderer/Camera.hpp"
#include "harmonia/scene/SceneLoader.hpp"

namespace harmonia::renderer {

/// Shared scene-camera assembly (CAM-1).
///
/// Scene parsing lives in harmonia::scene; this helper owns the interpretation
/// of the parsed camera fields so Theia and Hyperion cannot grow independent
/// fallback semantics. It returns the shared Camera::Params vocabulary and
/// deliberately does not touch renderer-specific camera controllers, jitter,
/// accumulation gates or presentation behavior.
class CameraAssembly {
  public:
    /// Assemble the static scene camera from the loader's resolved config.
    /// Returns std::nullopt when the camera-presence contract is not satisfied
    /// (SceneLoader::load already rejects those scenes before a renderer sees
    /// them; this remains the fail-closed seam for direct API callers).
    [[nodiscard]] static std::optional<Camera::Params>
    fromSceneConfig(const harmonia::scene::SceneLoader::SceneConfig& config, float aspectRatio) noexcept;

    /// Re-place an already-assembled camera from an evaluated ANI8 camera pose.
    /// Position/target/up and scene-scale near/far follow the shared static
    /// convention; lens, focus and physical-camera state are preserved.
    [[nodiscard]] static Camera::Params fromPose(const Camera::Params& base, const aether::CameraPose& pose) noexcept;
};

} // namespace harmonia::renderer

#endif // HARMONIA_RENDERER_CAMERAASSEMBLY_HPP
