#ifndef HARMONIA_SCENE_SKINDEFORMPASS_HPP
#define HARMONIA_SCENE_SKINDEFORMPASS_HPP

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

/// ANI4 renderer half - the shared per-frame GPU skinning pass (the compute
/// kernel is `shaders/skin_deform.comp.slang`, matched against Aether's
/// `deformSkinLbs` CPU reference):
///
///   1. stage the evaluated skin matrices (SceneBase::stageSkinMatrices) and
///      upload them with vkCmdUpdateBuffer (TRANSFER -> COMPUTE barrier);
///   2. dispatch one LBS job per skinned mesh: read the immutable REST-vertex
///      copy, write the deformed pose into the SCENE-GLOBAL vertex buffer
///      (what every shading stage reads; reading rest keeps the pass
///      idempotent - re-running replaces the pose, never composes onto the
///      previous frame);
///   3. copy each deformed range into the mesh-local vertex buffer (the BLAS
///      build input; COMPUTE -> TRANSFER, then TRANSFER -> AS-build/shader);
///   4. fence-wait (one-shot) and refit the dynamic BLASes in place
///      (SceneBase::refitDynamicBlases).
///
/// The pass owns the compute pipeline + the descriptor-buffer writer (bindings
/// 0-3 are the scene's skin buffers + the renderer's global vertex buffer,
/// written once at create - both are build-time static). The caller supplies
/// the per-mesh ranges into the scene-global buffer (renderer-specific layout:
/// `meshSceneVertexOffsets[meshIndex]`) and runs execute once per animation
/// tick, before `updateTlas`.
class SkinDeformPass {
  public:
    /// Creates the pipeline + writer. Requires a built skin-carrying scene
    /// (jobs + skin buffers exist) - call after SceneBase::build.
    /// @param restVertices the renderer's immutable REST-pose vertex copy (the
    ///   kernel's read source - the active buffer holds the previous pose);
    /// @param sceneVertices the renderer's ACTIVE scene-global vertex buffer.
    [[nodiscard]] static std::expected<SkinDeformPass, VkResult> create(const DeviceContext& ctx,
                                                                        SceneBase& scene,
                                                                        const core::Buffer& restVertices,
                                                                        const core::Buffer& sceneVertices,
                                                                        const std::filesystem::path& computeSpvPath);

    SkinDeformPass() = default;
    SkinDeformPass(const SkinDeformPass&) = delete;
    SkinDeformPass& operator=(const SkinDeformPass&) = delete;
    SkinDeformPass(SkinDeformPass&&) noexcept = default;
    SkinDeformPass& operator=(SkinDeformPass&&) noexcept = default;
    ~SkinDeformPass() noexcept;

    /// Runs the whole sequence above for @p seconds (poses are evaluated from
    /// the attached source - deterministic per time). Success no-op when the
    /// scene carries no skin jobs. Returns the first Vulkan failure (staging,
    /// submission or refit).
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
#endif // HARMONIA_SCENE_SKINDEFORMPASS_HPP
