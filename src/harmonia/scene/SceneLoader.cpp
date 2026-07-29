#include "harmonia/scene/SceneLoader.hpp"

#include <array>
#include <limits>
#include <slang-math/slang-math.hpp>
#include <string>
#include <toml++/toml.hpp>
#include <unordered_map>
#include <utility>
#include <vector>

#include "aether/format/ObjImporter.hpp"
#include "aether/format/SceneParser.hpp"
#include "aether/types/MeshData.hpp"
#include "harmonia/core/Logger.hpp"
#include "harmonia/scene/MaterialLibrary.hpp"
#include "harmonia/scene/ProceduralGeometry.hpp"
#include "harmonia/scene/Texture.hpp"

namespace {

void applyStageTogglesFromTable(const toml::table& table, SceneLoader::SceneConfig& cfg) {
    if (const auto v = table["enable_accumulation_stage"].value<bool>()) {
        cfg.accumulationStageEnabled = *v;
    }
    if (const auto v = table["enable_denoiser_stage"].value<bool>()) {
        cfg.denoiserStageEnabled = *v;
    }
    if (const auto v = table["enable_tonemapper_stage"].value<bool>()) {
        cfg.tonemapperStageEnabled = *v;
    }

    if (const toml::table* stages = table["stages"].as_table()) {
        if (const auto v = (*stages)["accumulation"].value<bool>()) {
            cfg.accumulationStageEnabled = *v;
        }
        if (const auto v = (*stages)["denoiser"].value<bool>()) {
            cfg.denoiserStageEnabled = *v;
        }
        if (const auto v = (*stages)["tonemapper"].value<bool>()) {
            cfg.tonemapperStageEnabled = *v;
        }
    }
}

void applyPostTonemapOverrideFromTable(const toml::table& table, SceneLoader::SceneConfig& cfg) {
    if (const toml::table* postTonemap = table["post_tonemap"].as_table()) {
        if (const auto renderer = (*postTonemap)["renderer"].value<std::string>()) {
            cfg.postTonemapRenderer = *renderer;
        }
    }
}

void parseRenderStageToggles(const std::filesystem::path& sceneFile, SceneLoader::SceneConfig& cfg) {
    try {
        const toml::table root = toml::parse_file(sceneFile.string());
        const toml::table* render = root["render"].as_table();
        if (render == nullptr) {
            return;
        }

        if (const auto ref = (*render)["reference"].value<std::string>()) {
            try {
                const std::filesystem::path refPath = sceneFile.parent_path() / *ref;
                const toml::table renderPreset = toml::parse_file(refPath.string());
                applyStageTogglesFromTable(renderPreset, cfg);
                applyPostTonemapOverrideFromTable(renderPreset, cfg);
            } catch (const toml::parse_error&) {
                Logger::warn("SceneLoader: malformed render preset reference '{}'", *ref);
            }
        }

        applyStageTogglesFromTable(*render, cfg);
        applyPostTonemapOverrideFromTable(*render, cfg);
    } catch (const toml::parse_error&) {
        Logger::warn("SceneLoader: failed to parse '{}' for stage toggles", sceneFile.string());
    }
}

// Build an Xform (object→world, glTF T × R × S) from a parsed instance's TRS.
[[nodiscard]] Xform toXform(const aether::InstanceDesc& inst) {
    return Xform{.translation = inst.translation, .rotation = inst.rotation, .scale = inst.scale};
}

// Convert Aether's renderer-agnostic object-space mesh into Harmonia's GpuVertex
// layout. OBJ vertex tangents are zero (mirrors the previous importer; tangents
// are derived per-hit in the shaders).
[[nodiscard]] MeshData toHarmoniaMesh(const aether::MeshData& am) {
    MeshData out;
    out.vertices.reserve(am.vertices.size());
    out.indices = am.indices;
    for (const aether::Vertex& v : am.vertices) {
        out.vertices.push_back(GpuVertex{
            .position = v.position,
            .tangentX = 0.0f,
            .normal = v.normal,
            .tangentY = 0.0f,
            .uv = v.uv,
            .tangentZ = 0.0f,
            .bitangentSign = 1.0f,
        });
    }
    return out;
}

/// One sub-mesh of a declared mesh: its registered mesh index + the OBJ group
/// name used to resolve per-instance material overrides.
struct LoadedSubmesh {
    std::uint32_t meshIndex{};
    std::string groupName;
};

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
    parseRenderStageToggles(sceneFile, cfg);
    MaterialLibrary lib;
    std::unordered_map<std::string, std::uint32_t> texCache; // relPath → texture index

    // ── Working color space ───────────────────────────────────────────────────
    if (desc->workingColorSpace) {
        if (const auto ws = ColorSpace::parseWorkingColorSpace(*desc->workingColorSpace))
            cfg.workingColorSpace = *ws;
        else
            Logger::warn("SceneLoader: unknown working_color_space '{}' — using lin_rec2020_scene",
                         *desc->workingColorSpace);
    }

    // ── Material libraries ────────────────────────────────────────────────────
    for (const std::string& mtllib : desc->mtllibs) {
        if (!lib.load(assetsDir / mtllib, cfg.workingColorSpace))
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
    // The camera's placement is a plain TRS transform — identical in shape to an
    // instance's. Forward/up are derived from the rotation quaternion.
    const aether::CameraDesc& cam = desc->camera;
    if (cam.translation)
        cfg.cameraPos = *cam.translation;
    if (cam.rotation) {
        const sm::float3 pos = cam.translation.value_or(sm::float3{0.0f, 0.0f, 0.0f});
        const sm::quaternion rot = *cam.rotation;
        cfg.cameraAt = pos + (rot * sm::float3{0.0f, 0.0f, -1.0f});
        cfg.cameraUp = rot * sm::float3{0.0f, 1.0f, 0.0f};
    }
    if (cam.vfov)
        cfg.cameraVfov = *cam.vfov;
    if (cam.ev100)
        cfg.cameraEv100 = *cam.ev100;

    // ── Texture loading (idempotent; cached by relative path) ─────────────────
    // Must run before the matching addMaterial() so patched bindless indices are
    // visible when lib.getOrDefault() is called.
    auto loadMatTextures = [&](const std::string& matName) {
        if (matName.empty())
            return;
        const auto refs = lib.textureRefs(matName);
        if (!refs)
            return;

        const std::array<std::pair<std::uint32_t, const MaterialLibrary::MaterialTextureRef*>, 7> slots{{
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
                lib.patchTextureIndex(matName, slot, it->second);
                continue;
            }

            auto result =
                Texture::loadFromFile(ctx, pool, assetsDir / relPath, ref->colorSpace, cfg.workingColorSpace, matName);
            if (!result) {
                Logger::warn("SceneLoader: failed to load texture '{}' for material '{}'", relPath, matName);
                continue;
            }

            const std::uint32_t idx = scene.addTexture(std::move(*result));
            texCache.emplace(relPath, idx);
            lib.patchTextureIndex(matName, slot, idx);
            Logger::info("SceneLoader: loaded texture '{}' (slot {}) → index {}", relPath, slot, idx);
        }
    };

    // ── Meshes: import each declared mesh ONCE (object space) ─────────────────
    // Records, per declared mesh name, the registered sub-mesh indices (+ OBJ
    // group names for material resolution). Instances reference these.
    std::unordered_map<std::string, std::vector<LoadedSubmesh>> meshSubmeshes;

    for (const aether::MeshDesc& m : desc->meshes) {
        std::vector<LoadedSubmesh> subs;

        switch (m.kind) {
        case aether::MeshDesc::Kind::Object: {
            if (m.objPath.empty()) {
                Logger::warn("SceneLoader: mesh '{}' has empty path — skipping", m.name);
                continue;
            }
            auto groups = aether::ObjImporter::parse(assetsDir / m.objPath);
            if (!groups) {
                Logger::error("SceneLoader: cannot open mesh '{}'", m.objPath);
                return std::nullopt;
            }
            for (const aether::MeshGroup& g : *groups) {
                if (g.mesh.empty()) {
                    continue;
                }
                const std::string debugName = m.name + "." + g.name;
                const std::uint32_t idx = scene.addMesh(ctx, pool, toHarmoniaMesh(g.mesh), debugName);
                if (idx == std::numeric_limits<std::uint32_t>::max()) {
                    Logger::error("SceneLoader: failed to upload mesh '{}'", debugName);
                    return std::nullopt;
                }
                subs.push_back({idx, g.name});
            }
            break;
        }
        case aether::MeshDesc::Kind::Box: {
            if (m.boxHalf == sm::float3{0.0f, 0.0f, 0.0f}) {
                Logger::warn("SceneLoader: box '{}' has zero half-extents — skipping", m.name);
                continue;
            }
            MeshData mesh = ProceduralGeometry::makeBox(m.boxHalf); // object space, no bake
            const std::uint32_t idx = scene.addMesh(ctx, pool, std::move(mesh), m.name);
            if (idx == std::numeric_limits<std::uint32_t>::max()) {
                Logger::error("SceneLoader: failed to upload box '{}'", m.name);
                return std::nullopt;
            }
            subs.push_back({idx, ""});
            break;
        }
        case aether::MeshDesc::Kind::Sphere: {
            if (m.sphereRadius <= 0.0f) {
                Logger::warn("SceneLoader: sphere '{}' has radius ≤ 0 — skipping", m.name);
                continue;
            }
            const std::uint32_t idx = scene.addSphereMesh(ctx, pool, m.sphereRadius, m.name);
            if (idx == std::numeric_limits<std::uint32_t>::max()) {
                Logger::error("SceneLoader: failed to upload sphere '{}'", m.name);
                return std::nullopt;
            }
            subs.push_back({idx, ""});
            break;
        }
        }

        if (subs.empty()) {
            Logger::warn("SceneLoader: mesh '{}' produced no geometry — skipping", m.name);
            continue;
        }
        meshSubmeshes.emplace(m.name, std::move(subs));
    }

    // ── Instances: place a mesh with a transform + material ───────────────────
    for (const aether::InstanceDesc& inst : desc->instances) {
        auto it = meshSubmeshes.find(inst.meshName);
        if (it == meshSubmeshes.end()) {
            Logger::warn("SceneLoader: instance references unknown mesh '{}' — skipping", inst.meshName);
            continue;
        }
        const Xform xform = toXform(inst);

        for (const LoadedSubmesh& sub : it->second) {
            // Material priority: per-group override > whole-instance override > default.
            std::string matName;
            if (!sub.groupName.empty()) {
                if (const auto git = inst.groupMaterials.find(sub.groupName); git != inst.groupMaterials.end())
                    matName = git->second;
                else if (!inst.materialName.empty())
                    matName = inst.materialName;
            } else {
                matName = inst.materialName;
            }

            loadMatTextures(matName);
            const std::uint32_t matIdx = scene.addMaterial(lib.getOrDefault(matName));
            if (scene.addInstance(sub.meshIndex, xform, matIdx) == std::numeric_limits<std::uint32_t>::max()) {
                Logger::error("SceneLoader: failed to place instance of mesh '{}'", inst.meshName);
                return std::nullopt;
            }
        }
    }

    Logger::info("SceneLoader: loaded '{}'", sceneFile.filename().string());
    return cfg;
}
