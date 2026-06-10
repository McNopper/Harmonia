#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include "harmonia/scene/SceneLoader.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>

#include "aether/format/SceneParser.hpp"
#include "harmonia/core/Logger.hpp"
#include "harmonia/scene/ISceneImporter.hpp"
#include "harmonia/scene/MaterialLibrary.hpp"
#include "harmonia/scene/ObjImporter.hpp"
#include "harmonia/scene/ProceduralGeometry.hpp"
#include "harmonia/scene/Texture.hpp"

namespace {

// Build a 4×4 TRS matrix from the geometry block's T/R/S fields.
[[nodiscard]] glm::mat4 trsMatrix(const aether::GeometryBlock& b) {
    return glm::translate(glm::mat4(1.0f), b.translation) * glm::mat4_cast(b.rotation) *
           glm::scale(glm::mat4(1.0f), b.scale);
}

// ── Geometry-block uploader ───────────────────────────────────────────────────

[[nodiscard]] bool uploadBlock(const aether::GeometryBlock& blk,
                              ISceneBuilder& scene,
                              const DeviceContext& ctx,
                              const CommandPool& pool,
                              MaterialLibrary& lib,
                              const std::filesystem::path& assetsDir,
                              std::unordered_map<std::string, uint32_t>& texCache) {

    // Pre-load textures for all materials referenced in this block.
    // Must run before ObjImporter/addMaterial so that patched texture indices
    // are visible when lib.getOrDefault() is called.
    auto loadMatTextures = [&](const std::string& matName) {
        if (matName.empty())
            return;
        const auto refs = lib.textureRefs(matName);
        if (!refs)
            return;

        // Slot order matches GpuMaterial textures: [0-3] base_color, normal, ORM, emission;
        // [4-6] coat_normal, tangent, coat_tangent (textureIndices2).
        const std::array<std::pair<uint32_t, const MaterialLibrary::MaterialTextureRef*>, 7> slots{{
            {0u, &refs->base_color},
            {1u, &refs->normal},
            {2u, &refs->orm},
            {3u, &refs->emission},
            {4u, &refs->coat_normal},
            {5u, &refs->tangent},
            {6u, &refs->coat_tangent},
        }};

        for (const auto& [slot, ref] : slots) {
            if (ref->empty())
                continue;

            const std::string& relPath = ref->path;
            if (const auto it = texCache.find(relPath); it != texCache.end()) {
                // Already uploaded — just patch the index.
                lib.patchTextureIndex(matName, slot, it->second);
                continue;
            }

            auto result = Texture::loadFromFile(ctx, pool, assetsDir / relPath, ref->colorSpace, matName);
            if (!result) {
                Logger::warn("SceneLoader: failed to load texture '{}' for material '{}'", relPath, matName);
                continue;
            }

            const uint32_t idx = scene.addTexture(std::move(*result));
            texCache.emplace(relPath, idx);
            lib.patchTextureIndex(matName, slot, idx);
            Logger::info("SceneLoader: loaded texture '{}' (slot {}) → index {}", relPath, slot, idx);
        }
    };

    loadMatTextures(blk.materialName);
    for (const auto& [groupName, matName] : blk.groupMaterials)
        loadMatTextures(matName);

    switch (blk.kind) {
    case aether::GeometryBlock::Kind::Object: {
        if (blk.objPath.empty()) {
            Logger::warn("SceneLoader: empty object path — skipping");
            return true;
        }
        ObjImporter importer;
        return importer.import(assetsDir / blk.objPath,
                               scene,
                               ctx,
                               pool,
                               ImportOptions{
                                   .worldTransform = trsMatrix(blk),
                                   .library = &lib,
                                   .overrideMaterial = blk.materialName,
                                   .groupMaterials = blk.groupMaterials,
                               });
    }

    case aether::GeometryBlock::Kind::Sphere: {
        if (blk.sphereRadius <= 0.0f) {
            Logger::warn("SceneLoader: sphere radius ≤ 0 — skipping");
            return true;
        }
        // scale.x used as a uniform radius multiplier (all axes equal for a sphere).
        // The renderer's Scene::addSphere decides analytic vs tessellated.
        const float radius = blk.sphereRadius * blk.scale.x;
        const uint32_t mat = scene.addMaterial(lib.getOrDefault(blk.materialName));
        return scene.addSphere(ctx, pool, blk.translation, radius, mat) != std::numeric_limits<uint32_t>::max();
    }

    case aether::GeometryBlock::Kind::Box: {
        if (blk.boxHalf == glm::vec3(0.0f)) {
            Logger::warn("SceneLoader: box half-extents are zero — skipping");
            return true;
        }
        const uint32_t mat = scene.addMaterial(lib.getOrDefault(blk.materialName));
        MeshData mesh = ProceduralGeometry::makeBox(blk.boxHalf, trsMatrix(blk));
        return scene.addMesh(ctx, pool, std::move(mesh), mat, "box") != std::numeric_limits<uint32_t>::max();
    }
    }
    return true; // unreachable
}

} // namespace

// ── SceneLoader::load ─────────────────────────────────────────────────────────

std::optional<SceneLoader::SceneConfig> SceneLoader::load(const std::filesystem::path& sceneFile,
                                                          const std::filesystem::path& assetsDir,
                                                          ISceneBuilder& scene,
                                                          const DeviceContext& ctx,
                                                          const CommandPool& pool) {
    // Parsing is owned by Aether — SceneLoader only resolves and uploads.
    const std::optional<aether::SceneDesc> desc = aether::SceneParser::parse(sceneFile);
    if (!desc) {
        Logger::error("SceneLoader: cannot open '{}'", sceneFile.string());
        return std::nullopt;
    }

    SceneConfig cfg{};
    MaterialLibrary lib;
    std::unordered_map<std::string, uint32_t> texCache; // relPath → texture index

    // ── Material libraries ────────────────────────────────────────────────────
    for (const std::string& mtllib : desc->mtllibs) {
        if (!lib.load(assetsDir / mtllib))
            Logger::warn("SceneLoader: cannot open material library '{}'", mtllib);
    }

    // ── Render settings ───────────────────────────────────────────────────────
    cfg.spp = desc->spp;
    cfg.maxDepth = desc->maxDepth;
    cfg.envUnitNits = desc->envUnitNits;
    if (desc->envMapFile)
        cfg.envMapFile = std::filesystem::path(*desc->envMapFile);

    if (desc->tonemapper) {
        const std::string& name = *desc->tonemapper;
        if (name == "aces")
            cfg.tonemapper = 0u;
        else if (name == "agx")
            cfg.tonemapper = 1u;
        else if (name == "reinhard")
            cfg.tonemapper = 2u;
        else if (name == "hable")
            cfg.tonemapper = 3u;
        else
            Logger::warn("SceneLoader: unknown tonemapper '{}' — using default (aces)", name);
    }

    // ── Camera ────────────────────────────────────────────────────────────────
    // The parser already resolved rotate / rotate_y / look_at into a single
    // look-at + up representation (last-one-wins).
    const aether::CameraDesc& cam = desc->camera;
    if (cam.position)
        cfg.cameraPos = *cam.position;
    if (cam.lookAt) {
        cfg.cameraAt = *cam.lookAt;
        cfg.cameraUp = cam.up.value_or(glm::vec3(0.0f, 1.0f, 0.0f));
    }
    if (cam.vfov)
        cfg.cameraVfov = *cam.vfov;
    if (cam.ev100)
        cfg.cameraEv100 = *cam.ev100;

    // ── Geometry ──────────────────────────────────────────────────────────────
    for (const aether::GeometryBlock& blk : desc->geometry) {
        if (!uploadBlock(blk, scene, ctx, pool, lib, assetsDir, texCache))
            return std::nullopt;
    }

    Logger::info("SceneLoader: loaded '{}'", sceneFile.filename().string());
    return cfg;
}
