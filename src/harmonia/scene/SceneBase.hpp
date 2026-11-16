#ifndef HARMONIA_SCENE_SCENEBASE_HPP
#define HARMONIA_SCENE_SCENEBASE_HPP

#include <volk/volk.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "aether/format/SceneAnimator.hpp"
#include "aether/types/OpacityMicromap.hpp"
#include "aether/types/SceneDesc.hpp"
#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/Buffer.hpp"
#include "harmonia/core/CommandPool.hpp"
#include "harmonia/scene/Geometry.hpp"
#include "harmonia/scene/ISceneBuilder.hpp"
#include "harmonia/scene/Light.hpp"
#include "harmonia/scene/Material.hpp"
#include "harmonia/scene/Mesh.hpp"
#include "harmonia/scene/Texture.hpp"

namespace harmonia::scene {

/// ANI4+ANI5: one GPU vertex-deformation job - the deformation pass dispatches
/// once per job, over ONE sub-mesh range. Both halves are optional and merge
/// onto the same job when a mesh is skinned AND morped (glTF order: the kernel
/// applies the morph deltas first, then skins the morphed position):
///   * skin  (ANI4): weightRow/matrixOffset/jointCount (jointCount 0 = none);
///   * morph (ANI5): morphDeltaOffset/Stride/TargetCount + the staged
///     morphWeightOffset (morphTargetCount 0 = none).
/// The absolute offsets are materialized by SceneBase::build from the loader's
/// `bindSkinMesh` / `bindMorphMesh` bindings (a skin's weight rows span the
/// covered sub-meshes in import order; morph regions are per declared mesh,
/// targets contiguous; both are shared by every job they drive).
struct DeformJob {
    std::uint32_t meshIndex = 0;   ///< scene mesh (Geometry) index
    std::uint32_t vertexCount = 0; ///< sub-mesh vertices this job deforms
    // ANI4 skin half
    std::uint32_t weightRow = 0;    ///< absolute row into the skin index/weight buffers
    std::uint32_t matrixOffset = 0; ///< this skin's first matrix, in float4x4 elements
    std::uint32_t jointCount = 0;   ///< matrices of this skin (0 = morph-only job)
    std::uint32_t useDqs = 0;       ///< 1 = dual-quaternion blend (SkinDesc::Blend::Dqs;
                                    ///< rigid - drops scale; shares matrixOffset as the
                                    ///< joint base into the DQ buffer)
    // ANI5 morph half
    std::uint32_t morphDeltaOffset = 0;  ///< element offset of target-0 row 0
    std::uint32_t morphDeltaStride = 0;  ///< elements per target (declared mesh count)
    std::uint32_t morphTargetCount = 0;  ///< targets of this job (0 = skin-only job)
    std::uint32_t morphWeightOffset = 0; ///< float offset into the staged weight buffer
};

/// Non-renderer-specific base for both Hyperion::Scene and Theia::Scene.
///
/// Provides concrete, identical implementations of the operations that do not
/// depend on the GPU layout (addMaterial / addTexture / addLight / addInstance /
/// addMesh / build) and owns the corresponding storage vectors.  Renderer-specific
/// work (addSphereMesh, buildSceneBuffers) is deferred to the derived class via the
/// protected virtual hooks below; the ray-tracing build (BLAS + TLAS) is provided
/// by the harmonia::scene_rt module, which the derived scenes call from
/// finishBuild.
///
/// Storage follows the mesh/instance split: `m_meshes` holds unique geometry and
/// `m_instances` holds the placements (mesh index + transform + material).
/// N instances of one mesh share that mesh's BLAS.
class SceneBase : public ISceneBuilder {
  public:
    // ── ISceneBuilder (partial) ──────────────────────────────────────────────

    /// Appends @p mat to the material list and returns its index.
    [[nodiscard]] std::uint32_t addMaterial(Material&& mat) override;

    /// Appends @p texture to the bindless-texture list and returns its index.
    [[nodiscard]] std::uint32_t addTexture(Texture&& texture) override;

    /// Records an instance placement (mesh index + transform + material). Shared
    /// because it is pure bookkeeping — no GPU work until build().
    [[nodiscard]] std::uint32_t
    addInstance(std::uint32_t meshIndex, const Transform& transform, std::uint32_t materialIdx) override;

    /// Registers a unique triangle mesh (object space). Shared because it is
    /// identical across renderers (a plain TriangleMesh upload).
    [[nodiscard]] std::uint32_t addMesh(const DeviceContext& ctx,
                                        const core::CommandPool& pool,
                                        MeshData&& data,
                                        const MeshOpacity& opacity,
                                        std::string_view name = "") override;

    // ── Additional scene population ─────────────────────────────────────────

    /// Takes ownership of a parsed opacity-micromap asset and returns a stable
    /// reference to it. The scene keeps the asset alive for as long as its
    /// meshes (whose `buildBlas` reads the group), so callers may hand out
    /// `OpacityMicromapGroup*` pointers into the returned data.
    [[nodiscard]] const aether::OpacityMicromapData& addOpacityMicromap(aether::OpacityMicromapData&& data) override;

    /// Appends @p light and returns its index.  Must be called before build().
    std::uint32_t addLight(std::unique_ptr<Light> light);

    // ── Build (shared orchestration) ────────────────────────────────────────

    /// Uploads scene buffers (buildSceneBuffers), builds one BLAS per unique mesh
    /// (via the mesh's own buildBlas), then hands the fully uploaded scene to the
    /// finishBuild hook - which the scene_rt module backs with the TLAS build.
    /// Shared because this orchestration is identical across renderers; only the
    /// virtual hooks diverge. Returns VK_ERROR_INITIALIZATION_FAILED when no
    /// instances were added.
    VkResult build(const DeviceContext& ctx, const core::CommandPool& pool);

    // ── Animation (ANI1/ANI2 bridge — shared Aether consumption) ─────────────

    /// True when an animated-scene source with TRACKS was attached (SceneLoader
    /// attaches the source for `[[skin]]`-only scenes too - ANI4 - so "animated"
    /// means the source actually declares `[[animation]]` tracks; skin-only
    /// sources never tick the animation path).
    [[nodiscard]] bool hasAnimation() const noexcept {
        return m_animation.has_value() && !m_animation->animations.empty();
    }

    /// The attached scene's animation clock rate (ticks per second, Aether
    /// ANI2). Meaningful only when hasAnimation().
    [[nodiscard]] double animationFps() const noexcept { return m_animation ? m_animation->animationFps : 30.0; }

    /// Evaluates the attached animation tracks at @p seconds and stamps the
    /// resolved TRS onto the scene instances through the recorded
    /// sceneInstance → descInstance mapping, then marks the instances dirty
    /// (the TLAS update path consumes the flag). No-op without an attached
    /// source. Deterministic: the same time always stamps the same transforms.
    void setAnimationTime(float seconds);

    /// ANI4: true when the attached source declares `[[skin]]` bindings (a
    /// skin-only scene attaches its source too - see SceneLoader).
    [[nodiscard]] bool hasSkins() const noexcept { return m_animation.has_value() && !m_animation->skins.empty(); }

    /// ANI4: evaluate the attached skin poses at @p seconds - the LOCAL-space
    /// skin matrices (Aether: `inverse(nodeWorld) * jointWorld * inverseBind`,
    /// one per joint in declaration order; see `aether::SkinPose`) that the
    /// deformation pass feeds to the BLAS refit. Empty without attached skins;
    /// deterministic (the same time always yields the same matrices).
    [[nodiscard]] std::vector<aether::SkinPose> evaluateSkins(float seconds) const;

    /// ANI4: refit every dynamic (skinned) mesh's BLAS in place - call AFTER the
    /// deformation pass rewrote its vertex buffer and BEFORE the TLAS update
    /// (`updateTlas`). No-op when no mesh is dynamic. Handle/device address stay
    /// valid across the refit.
    [[nodiscard]] VkResult refitDynamicBlases(const DeviceContext& ctx, const core::CommandPool& pool);

    /// ANI4: copy @p poses' skin matrices into the staging scratch (one region
    /// per declared skin, in declaration order; a pose missing for a declared
    /// skin is a hand-built-scene contract break and fails the call). Called by
    /// DeformPass::execute before it records the vkCmdUpdateBuffer upload.
    /// Success no-op on morph-only scenes (no matrices to stage).
    [[nodiscard]] VkResult stageSkinMatrices(const std::vector<aether::SkinPose>& poses);

    /// ANI5: true when the attached source declares `[[morph]]` targets (a
    /// morph-only scene attaches its source too - see SceneLoader).
    [[nodiscard]] bool hasMorphs() const noexcept { return m_animation.has_value() && !m_animation->morphs.empty(); }

    /// ANI5: evaluate the morph-weight tracks at @p seconds - the live weights
    /// parallel to the attached `morphs` (authored initials without tracks).
    /// Empty without attached morphs; deterministic per time. Pair with
    /// `DeformPass::execute`, which stages them each tick.
    [[nodiscard]] std::vector<float> evaluateMorphWeights(float seconds) const;

    /// ANI5: copy @p weights (parallel to the attached morphs) into the
    /// per-mesh staged-weight scratch. Called by DeformPass::execute. Success
    /// no-op on skin-only scenes.
    [[nodiscard]] VkResult stageMorphWeights(const std::vector<float>& weights);

    // ── ANI4+ANI5 deformation buffers/jobs (read by DeformPass) ─────────────

    [[nodiscard]] const std::vector<DeformJob>& deformJobs() const noexcept { return m_deformJobs; }
    /// Concatenated per-vertex joint indices (uint4 rows) + weights (float4
    /// rows) over the declared skins, in declaration order. Valid once build()
    /// ran on a skin-carrying scene (invalid - null-descriptor - otherwise).
    [[nodiscard]] const core::Buffer& skinIndexBuffer() const noexcept { return m_skinIndexBuffer; }
    [[nodiscard]] const core::Buffer& skinWeightBuffer() const noexcept { return m_skinWeightBuffer; }
    /// Per-frame skin matrices (row-major float4x4), uploaded through
    /// stageSkinMatrices' scratch by the deformation pass each tick.
    [[nodiscard]] const core::Buffer& skinMatrixBuffer() const noexcept { return m_skinMatrixBuffer; }
    [[nodiscard]] std::span<const std::byte> skinMatrixScratch() const noexcept { return m_skinMatrixScratch; }
    /// ANI5: pad-w position deltas, per declared mesh region (targets
    /// contiguous, per-mesh grouping in declaration order) - build-time static.
    [[nodiscard]] const core::Buffer& morphDeltaBuffer() const noexcept { return m_morphDeltaBuffer; }
    /// ANI5: per-mesh weight region, re-uploaded each tick through
    /// stageMorphWeights' scratch (one float per target of the mesh).
    [[nodiscard]] const core::Buffer& morphWeightBuffer() const noexcept { return m_morphWeightBuffer; }
    [[nodiscard]] std::span<const std::byte> morphWeightScratch() const noexcept { return m_morphWeightScratch; }
    /// ANI4-DQS: per-joint dual quaternions (8 floats per joint - real xyzw,
    /// dual xyzw), joint-parallel to the matrix buffer (the SAME joint base via
    /// matrixOffset). Allocated + staged only when some skin declares
    /// blend = "dqs" (null otherwise - the pass binds a null descriptor).
    [[nodiscard]] const core::Buffer& skinDqBuffer() const noexcept { return m_skinDqBuffer; }
    [[nodiscard]] std::span<const std::byte> skinDqScratch() const noexcept { return m_skinDqScratch; }
    /// The attached scene source (test/tooling seam: re-deriving the same
    /// SkinDesc/deformSkinLbs and MorphTarget/deformMorphs references).
    [[nodiscard]] const aether::SceneDesc* attachedScene() const noexcept {
        return m_animation ? &*m_animation : nullptr;
    }

    /// Direct instance-transform mutation for non-desc-driven updates
    /// (renderers/tests stamping world transforms themselves). Marks the
    /// instances dirty.
    [[nodiscard]] Transform& instanceTransform(std::size_t instanceIndex) noexcept {
        m_instancesDirty = true;
        return m_instances[instanceIndex].transform;
    }

    /// Manual dirty flag for bulk edits through other seams.
    void markInstancesDirty() noexcept { m_instancesDirty = true; }
    [[nodiscard]] bool instancesDirty() const noexcept { return m_instancesDirty; }
    void clearInstancesDirty() noexcept { m_instancesDirty = false; }

    // ── Accessors (read-only) ────────────────────────────────────────────────

    [[nodiscard]] const std::vector<Texture>& textures() const noexcept { return m_textures; }
    [[nodiscard]] const std::vector<std::unique_ptr<Geometry>>& meshes() const noexcept { return m_meshes; }
    [[nodiscard]] const std::vector<InstanceRecord>& instances() const noexcept { return m_instances; }

    /// Per-instance visibility mask for the TLAS instance (the scene_rt TLAS
    /// build reads it). Defaults to "visible to all".
    virtual std::uint32_t instanceMask(std::size_t instanceIndex) const;

  protected:
    /// Renderer-specific GPU buffer upload (vertices/indices/materials/emissive/…).
    /// Diverges: Hyperion lays out an index buffer + analytic spheres; Theia builds
    /// meshlets + per-instance visibility masks.
    virtual VkResult buildSceneBuffers(const DeviceContext& ctx, const core::CommandPool& pool) = 0;

    /// Creates the concrete (scene_rt) triangle-mesh implementation. Hook, because
    /// the scene module stays free of ray-tracing types.
    [[nodiscard]] virtual std::unique_ptr<Geometry> createTriangleMesh(const DeviceContext& ctx,
                                                                       const core::CommandPool& pool,
                                                                       MeshData&& data,
                                                                       const MeshOpacity& opacity,
                                                                       std::string_view name) = 0;

    /// Called by build() after the scene buffers and the per-mesh BLAS builds: the
    /// scene_rt module builds the TLAS here and the derived scene stores it.
    virtual VkResult finishBuild(const DeviceContext& ctx, const core::CommandPool& pool) = 0;

    /// ANI2 seam (ISceneBuilder): stores the parsed animated-scene source.
    void attachAnimation(aether::SceneDesc&& desc, std::vector<std::uint32_t> instanceToDesc) override;

    /// ANI4 seam (ISceneBuilder): marks mesh @p meshIndex per-frame deformed so
    /// its BLAS builds with ALLOW_UPDATE (the loader calls this per covered
    /// mesh, before SceneBase::build constructs the BLASes).
    void markMeshDynamic(std::uint32_t meshIndex) override;

    /// ANI4 seam (ISceneBuilder): records the loader's skin binding (sub-mesh
    /// vertex total + within-skin weight row); SceneBase::build materializes
    /// the absolute job fields from these.
    void bindSkinMesh(std::uint32_t meshIndex,
                      std::uint32_t skinIndex,
                      std::uint32_t weightRowOffset,
                      std::uint32_t vertexCount) override;

    /// ANI5 seam (ISceneBuilder): records the loader's morph binding (sub-mesh
    /// range within the declared mesh's vertex span); SceneBase::build groups
    /// the declared morph targets per mesh name and materializes the absolute
    /// delta/weight offsets.
    void bindMorphMesh(std::uint32_t meshIndex,
                       std::string_view meshName,
                       std::uint32_t subVertexOffset,
                       std::uint32_t subVertexCount) override;

    /// ANI4+ANI5 (called by build): materializes the merged DeformJobs from the
    /// pending loader bindings and uploads the skin index/weight/matrix +
    /// morph delta/weight buffers. No-op when nothing binds.
    [[nodiscard]] VkResult buildDeformBuffers(const DeviceContext& ctx, const core::CommandPool& pool);

    [[nodiscard]] static std::expected<core::Buffer, VkResult> uploadStorageBuffer(const DeviceContext& ctx,
                                                                                   const core::CommandPool& pool,
                                                                                   std::span<const std::byte> data,
                                                                                   std::string_view debugName,
                                                                                   VkBufferUsageFlags usage);

    std::vector<Material> m_materials;
    std::vector<std::unique_ptr<Geometry>> m_meshes; ///< unique meshes
    std::vector<InstanceRecord> m_instances;         ///< placements referencing m_meshes
    std::vector<std::unique_ptr<Light>> m_lights;
    std::vector<Texture> m_textures;
    /// Parsed OMM assets. A deque, not a vector: `addOpacityMicromap` hands out
    /// references into these and meshes keep `OpacityMicromapGroup*` pointers
    /// into them until `buildBlas`, so the storage must never reallocate.
    std::deque<aether::OpacityMicromapData> m_ommAssets;
    /// ANI2: the attached animated-scene source + sceneInstance → descInstance
    /// mapping (set by the loader for [[animation]] scenes; both optional).
    std::optional<aether::SceneDesc> m_animation;
    std::vector<std::uint32_t> m_instanceToDesc;
    /// Set by any instance-transform mutation; the TLAS update path consumes it.
    bool m_instancesDirty = false;

    /// ANI4: pending loader skin bindings (mesh, skin, within-skin row, count).
    std::vector<std::array<std::uint32_t, 4>> m_pendingSkinBindings;
    /// ANI5: pending loader morph bindings (sub-mesh range within the named
    /// declared mesh - the name groups the morph targets and their regions).
    struct PendingMorphBinding {
        std::uint32_t meshIndex = 0;
        std::string meshName;
        std::uint32_t subVertexOffset = 0;
        std::uint32_t subVertexCount = 0;
    };
    std::vector<PendingMorphBinding> m_pendingMorphBindings;
    std::vector<DeformJob> m_deformJobs; ///< merged absolute jobs (buildDeformBuffers)
    /// ANI5: per weight-region target indices (into the attached morphs), in
    /// staged order - stageMorphWeights copies through these each tick.
    std::vector<std::vector<std::uint32_t>> m_morphWeightGroups;
    core::Buffer m_skinIndexBuffer{};  ///< uint4 rows, STORAGE (null when skin-free)
    core::Buffer m_skinWeightBuffer{}; ///< float4 rows, STORAGE (null when skin-free)
    core::Buffer m_skinMatrixBuffer{}; ///< float4x4 rows, STORAGE + TRANSFER_DST
    /// Host staging for the current frame's matrices (byte size = matrix buffer).
    std::vector<std::byte> m_skinMatrixScratch;
    core::Buffer m_morphDeltaBuffer{};  ///< pad-w float4 rows, STORAGE (build-static)
    core::Buffer m_morphWeightBuffer{}; ///< one float per target per region, TRANSFER_DST
    /// Host staging for the current frame's morph weights.
    std::vector<std::byte> m_morphWeightScratch;
    /// ANI4-DQS: joint-major dual quaternions (2 float4 per joint) - build-time
    /// zero-filled, re-staged from the evaluated poses each tick when any skin
    /// declares blend = "dqs".
    core::Buffer m_skinDqBuffer{};
    std::vector<std::byte> m_skinDqScratch;
};

} // namespace harmonia::scene
#endif // HARMONIA_SCENE_SCENEBASE_HPP
