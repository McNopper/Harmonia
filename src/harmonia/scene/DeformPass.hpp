#ifndef HARMONIA_SCENE_DEFORMPASS_HPP
#define HARMONIA_SCENE_DEFORMPASS_HPP

#include <volk/volk.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/CommandPool.hpp"
#include "harmonia/core/DescriptorBufferWriter.hpp"
#include "harmonia/core/VulkanHandle.hpp"
#include "harmonia/scene/SceneBase.hpp"

namespace harmonia::scene {

/// ANI4+ANI5 renderer half - the shared per-frame VERTEX DEFORMATION pass
/// (formerly SkinDeformPass; renamed when ANI5 morph targets joined the
/// dispatch). The compute kernel is `shaders/deform.comp.slang`, matched
/// against Aether's `deformMorphs` + `deformSkinLbs` CPU references:
///
///   1. stage the per-tick inputs (SceneBase::stageSkinMatrices +
///      stageMorphWeights) and upload them with vkCmdUpdateBuffer
///      (TRANSFER -> COMPUTE barrier); static inputs (skin rows, morph deltas)
///      were uploaded at build;
///   2. dispatch ONE job per deformed sub-mesh: read the immutable REST copy,
///      apply the morph deltas (POSITION-only), skin the morphed position when
///      the job carries a skin (glTF order), write the ACTIVE scene-global
///      vertex buffer - reading rest keeps the pass idempotent: re-running at
///      a new tick REPLACES the pose, never composes onto the previous frame;
///   3. copy each deformed range into the mesh-local vertex buffer (the BLAS
///      build input; COMPUTE -> TRANSFER, then TRANSFER -> AS-build/shader);
///   4. fence-wait (one-shot) and refit the dynamic BLASes in place
///      (SceneBase::refitDynamicBlases).
///
/// The pass owns the compute pipeline + the descriptor-buffer writer (7
/// bindings: skin index/weight/matrix - null descriptors when the scene is
/// morph-only - rest vertices, active vertices, morph deltas, staged morph
/// weights; written once at create from the build-time buffers). The caller
/// supplies the per-mesh ranges into the scene-global buffer (renderer-specific
/// layout: `meshSceneVertexOffsets[meshIndex]`) and runs execute once per
/// animation tick, before `updateTlas`.
class DeformPass {
  public:
    /// Creates the pipeline + writer. Requires a built deforming scene (jobs +
    /// deformation buffers exist) - call after SceneBase::build.
    /// @param restVertices the renderer's immutable REST-pose vertex copy (the
    ///   kernel's read source - the active buffer holds the previous pose);
    /// @param sceneVertices the renderer's ACTIVE scene-global vertex buffer.
    [[nodiscard]] static std::expected<DeformPass, VkResult> create(const DeviceContext& ctx,
                                                                    SceneBase& scene,
                                                                    const core::Buffer& restVertices,
                                                                    const core::Buffer& sceneVertices,
                                                                    const std::filesystem::path& computeSpvPath);

    DeformPass() = default;
    DeformPass(const DeformPass&) = delete;
    DeformPass& operator=(const DeformPass&) = delete;
    DeformPass(DeformPass&&) noexcept = default;
    DeformPass& operator=(DeformPass&&) noexcept = default;
    ~DeformPass() noexcept;

    /// Runs the whole sequence above for @p seconds (poses + morph weights are
    /// evaluated from the attached source - deterministic per time). Success
    /// no-op when the scene carries no deformation jobs. Returns the first
    /// Vulkan failure (staging, submission or refit).
    [[nodiscard]] VkResult execute(const DeviceContext& ctx,
                                   const core::CommandPool& pool,
                                   SceneBase& scene,
                                   float seconds,
                                   const core::Buffer& restVertices,
                                   const core::Buffer& sceneVertices,
                                   std::span<const std::uint32_t> meshSceneVertexOffsets);

  private:
    const DeviceContext* m_ctx = nullptr;
    core::UniquePipeline m_pipeline;
    core::UniquePipelineLayout m_pipelineLayout;
    core::UniqueDescriptorSetLayout m_setLayout;
    core::DescriptorBufferWriter m_writer;
};

} // namespace harmonia::scene
#endif // HARMONIA_SCENE_DEFORMPASS_HPP
