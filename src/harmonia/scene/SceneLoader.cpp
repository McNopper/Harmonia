#include "harmonia/scene/SceneLoader.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <slang-math/slang-math.hpp>
#include <string>
#include <toml++/toml.hpp>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "aether/format/ObjImporter.hpp"
#include "aether/format/OmmImporter.hpp"
#include "aether/format/SceneParser.hpp"
#include "aether/types/MeshData.hpp"
#include "aether/types/OpacityMicromap.hpp"
#include "harmonia/core/Logger.hpp"
#include "harmonia/scene/MaterialLibrary.hpp"
#include "harmonia/scene/ProceduralGeometry.hpp"
#include "harmonia/scene/Texture.hpp"

namespace harmonia {

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

// Convert an authored object-space AABB (Aether → Harmonia). The authored bounds
// applies to the whole OBJ; each sub-mesh carries it as a conservative bounds
// (a superset of the sub-mesh's own vertices — safe for culling/rejection).
[[nodiscard]] std::optional<Aabb> toHarmoniaBounds(const std::optional<aether::Aabb>& b) {
    if (!b) {
        return std::nullopt;
    }
    return Aabb{.min = b->min, .max = b->max};
}

/// One sub-mesh of a declared mesh: its registered mesh index + the OBJ group
/// name used to resolve per-instance material overrides.
struct LoadedSubmesh {
    std::uint32_t meshIndex{};
    std::string groupName;
};

} // namespace

// ── SceneLoader::load ─────────────────────────────────────────────────────────

// ── Scene-load phases ─────────────────────────────────────────────────────────

namespace {

using SceneConfig = SceneLoader::SceneConfig;

/// Everything the scene-load phases share (Extract Class): the resource handles,
/// the material library, and the cross-phase caches.
struct SceneLoadContext {
    const std::filesystem::path& assetsDir;
    const DeviceContext& ctx;
    const CommandPool& pool;
    ISceneBuilder& scene;
    SceneConfig& cfg;
    MaterialLibrary lib{};
    std::unordered_map<std::string, std::uint32_t> texCache{};          // relPath → texture index
    std::unordered_map<std::string, bool> meshAlphaTested{};            // whole-mesh material
    std::unordered_set<std::string> groupAlphaTested{};                 // "<mesh>\0<group>"
    std::unordered_map<std::string, std::vector<LoadedSubmesh>> meshSubmeshes{};
};

[[nodiscard]] bool materialIsAlphaTested(const MaterialLibrary& lib, const std::string& name) {
    if (name.empty()) {
        return false;
    }
    if (const auto refs = lib.textureRefs(name); refs && !refs->opacity.empty()) {
        return true;
    }
    const auto mat = lib.get(name);
    return mat && mat->gpu().opacityFlagsPad.x < 1.0f;
}

void applyWorkingColorSpace(const aether::SceneDesc& desc, SceneConfig& cfg) {
    // ── Working color space ───────────────────────────────────────────────────
    if (desc.workingColorSpace) {
        if (const auto ws = harmonia::ColorSpace::parseWorkingColorSpace(*desc.workingColorSpace))
            cfg.workingColorSpace = *ws;
        else
            Logger::warn("SceneLoader: unknown working_color_space '{}' — using lin_rec2020_scene",
                         *desc.workingColorSpace);
    }
}

void loadMaterialLibraries(const aether::SceneDesc& desc,
                           const std::filesystem::path& assetsDir,
                           SceneConfig& cfg,
                           MaterialLibrary& lib) {
    // ── Material libraries ────────────────────────────────────────────────────
    for (const std::string& mtllib : desc.mtllibs) {
        if (!lib.load(assetsDir / mtllib, cfg.workingColorSpace))
            Logger::warn("SceneLoader: cannot open material library '{}'", mtllib);
    }
}

void applyRenderSettings(const aether::SceneDesc& desc, SceneConfig& cfg) {
    // ── Render settings ───────────────────────────────────────────────────────
    cfg.spp = desc.spp;
    cfg.maxDepth = desc.maxDepth;
    cfg.envUnitNits = desc.envUnitNits;
    if (desc.envMapFile)
        cfg.envMapFile = std::filesystem::path(*desc.envMapFile);

    if (desc.tonemapper) {
        const std::string& name = *desc.tonemapper;
        static const std::unordered_map<std::string, std::uint32_t> kTonemappers{
            {"aces", 0u}, {"agx", 1u}, {"reinhard", 2u}, {"hable", 3u}};
        if (const auto it = kTonemappers.find(name); it != kTonemappers.end()) {
            cfg.tonemapper = it->second;
        } else {
            Logger::warn("SceneLoader: unknown tonemapper '{}' — using default (aces)", name);
        }
    }
}

    // ── Camera ────────────────────────────────────────────────────────────────
    // The camera's placement is a plain TRS transform — identical in shape to an
    // instance's. Forward/up are derived from the rotation quaternion.
void applyCamera(const aether::CameraDesc& cam, SceneConfig& cfg) {
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
}

// ── Texture loading (idempotent; cached by relative path) ────────
// Must run before the matching addMaterial() so patched bindless indices are
// visible when lib.getOrDefault() is called.
void loadMaterialTextures(SceneLoadContext& c, const std::string& matName) {
    if (matName.empty())
        return;
    const auto refs = c.lib.textureRefs(matName);
    if (!refs)
        return;

    const std::array<std::pair<std::uint32_t, const MaterialLibrary::MaterialTextureRef*>, 8> slots{{
        {0u, &refs->base_color},
        {1u, &refs->normal},
        {2u, &refs->orm},
        {3u, &refs->emission},
        {4u, &refs->coat_normal},
        {5u, &refs->tangent},
        {6u, &refs->coat_tangent},
        {7u, &refs->opacity},
    }};

    for (const auto& [slot, ref] : slots) {
        if (ref->empty())
            continue;

        const std::string& relPath = ref->path;
        if (const auto it = c.texCache.find(relPath); it != c.texCache.end()) {
            c.lib.patchTextureIndex(matName, slot, it->second);
            continue;
        }

        auto result =
            Texture::loadFromFile(c.ctx, c.pool, c.assetsDir / relPath, ref->colorSpace, c.cfg.workingColorSpace, matName);
        if (!result) {
            Logger::warn("SceneLoader: failed to load texture '{}' for material '{}'", relPath, matName);
            continue;
        }

        const std::uint32_t idx = c.scene.addTexture(std::move(*result));
        c.texCache.emplace(relPath, idx);
        c.lib.patchTextureIndex(matName, slot, idx);
        Logger::info("SceneLoader: loaded texture '{}' (slot {}) → index {}", relPath, slot, idx);
    }
}

// ── Cutout participation (must be known BEFORE the BLAS is built) ──────
// A mesh whose material can be less than fully present (`geometry_opacity < 1`
// or a `map_opacity` mask) must build non-opaque BLAS geometry, otherwise the
// shadow any-hit never runs and the cutout casts a solid shadow. Materials are
// attached by instances, which are processed after the meshes, so walk the
// instance list first and record which (mesh, OBJ group) pairs are affected.
// Erring towards "alpha-tested" only costs traversal speed, never correctness.
void computeAlphaTested(SceneLoadContext& c, const aether::SceneDesc& desc) {
    for (const aether::InstanceDesc& inst : desc.instances) {
        if (materialIsAlphaTested(c.lib, inst.materialName)) {
            c.meshAlphaTested[inst.meshName] = true;
        }
        for (const auto& [groupName, matName] : inst.groupMaterials) {
            if (materialIsAlphaTested(c.lib, matName)) {
                c.groupAlphaTested.insert(inst.meshName + '\0' + groupName);
            }
        }
    }
}

[[nodiscard]] bool isAlphaTested(const SceneLoadContext& c,
                                 const std::string& meshName,
                                 const std::string& groupName) {
    return c.meshAlphaTested.contains(meshName) || c.groupAlphaTested.contains(meshName + '\0' + groupName);
}

// Resolve the opacity-micromap references of one mesh: fail fast without
// VK_EXT_opacity_micromap, parse each referenced .micromap.toml once (dedup by
// path) and map OBJ group name -> parsed group. The scene owns the parsed assets.
[[nodiscard]] std::expected<std::unordered_map<std::string, const aether::OpacityMicromapGroup*>, VkResult>
resolveMicromaps(SceneLoadContext& c, const aether::MeshDesc& m) {
    // Opacity-micromap references: a scene that uses an OMM requires
    // VK_EXT_opacity_micromap — there is no opaque fallback (a cutout is not
    // an image-identical absent-branch). Fail fast if the device lacks it.
    if (!m.opacityMicromaps.empty() && !c.ctx.opacityMicromapSupported) {
        Logger::error("SceneLoader: mesh '{}' uses opacity_micromaps but the device "
                      "lacks VK_EXT_opacity_micromap",
                      m.name);
        return std::unexpected(VK_ERROR_FEATURE_NOT_PRESENT);
    }
    // Resolve each referenced .micromap.toml once (dedup by path) and map
    // OBJ group name -> parsed group. The scene owns the parsed assets.
    std::unordered_map<std::string, const aether::OpacityMicromapGroup*> ommForGroup;
    std::unordered_map<std::string, const aether::OpacityMicromapData*> parsedOmmFiles;
    for (const auto& [groupName, relPath] : m.opacityMicromaps) {
        const std::filesystem::path ommPath = c.assetsDir / relPath;
        const std::string key = ommPath.string();
        const aether::OpacityMicromapData* asset = parsedOmmFiles[key];
        if (asset == nullptr) {
            auto parsed = aether::OmmImporter::parse(ommPath);
            if (!parsed) {
                Logger::error("SceneLoader: cannot parse opacity micromap '{}'", relPath);
                return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
            }
            asset = &c.scene.addOpacityMicromap(std::move(*parsed));
            parsedOmmFiles[key] = asset;
        }
        for (const auto& ag : asset->groups) {
            if (ag.name == groupName) {
                ommForGroup[groupName] = &ag;
                break;
            }
        }
        if (ommForGroup.find(groupName) == ommForGroup.end()) {
            Logger::error("SceneLoader: opacity micromap '{}' has no group '{}'", relPath, groupName);
            return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
        }
    }

    return ommForGroup;
}

using MicromapGroups = std::unordered_map<std::string, const aether::OpacityMicromapGroup*>;
using ImportedSubmeshes = std::optional<std::vector<LoadedSubmesh>>;

[[nodiscard]] std::expected<ImportedSubmeshes, VkResult> importObjectMesh(
    SceneLoadContext& c,
    const aether::MeshDesc& m,
    MicromapGroups& ommForGroup) {
    std::vector<LoadedSubmesh> subs;
    if (m.objPath.empty()) {
        Logger::warn("SceneLoader: mesh '{}' has empty path — skipping", m.name);
        return std::nullopt;
    }
    auto groups = aether::ObjImporter::parse(c.assetsDir / m.objPath);
    if (!groups) {
        Logger::error("SceneLoader: cannot open mesh '{}'", m.objPath);
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }
    for (const aether::MeshGroup& g : *groups) {
        if (g.mesh.empty()) {
            continue;
        }
        const std::string debugName = m.name + "." + g.name;
        MeshData hmesh = toHarmoniaMesh(g.mesh);
        hmesh.bounds = toHarmoniaBounds(m.bounds);
        const MeshOpacity opacity{
            .alphaTested = isAlphaTested(c, m.name, g.name),
            .micromap = ommForGroup[g.name],
        };
        const std::uint32_t idx = c.scene.addMesh(c.ctx, c.pool, std::move(hmesh), opacity, debugName);
        if (idx == std::numeric_limits<std::uint32_t>::max()) {
            Logger::error("SceneLoader: failed to upload mesh '{}'", debugName);
            return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
        }
        subs.push_back({idx, g.name});
    }
    return subs;

}

[[nodiscard]] std::expected<ImportedSubmeshes, VkResult> importBoxMesh(SceneLoadContext& c,
                                                                       const aether::MeshDesc& m) {
    std::vector<LoadedSubmesh> subs;
    if (m.boxHalf == sm::float3{0.0f, 0.0f, 0.0f}) {
        Logger::warn("SceneLoader: box '{}' has zero half-extents — skipping", m.name);
        return std::nullopt;
    }
    MeshData mesh = harmonia::ProceduralGeometry::makeBox(m.boxHalf); // object space, no bake
    const std::uint32_t idx = c.scene.addMesh(
        c.ctx, c.pool, std::move(mesh), MeshOpacity{.alphaTested = isAlphaTested(c, m.name, "")}, m.name);
    if (idx == std::numeric_limits<std::uint32_t>::max()) {
        Logger::error("SceneLoader: failed to upload box '{}'", m.name);
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }
    subs.push_back({idx, ""});
    return subs;

}

[[nodiscard]] std::expected<ImportedSubmeshes, VkResult> importSphereMesh(SceneLoadContext& c,
                                                                          const aether::MeshDesc& m) {
    std::vector<LoadedSubmesh> subs;
    if (m.sphereRadius <= 0.0f) {
        Logger::warn("SceneLoader: sphere '{}' has radius ≤ 0 — skipping", m.name);
        return std::nullopt;
    }
    const std::uint32_t idx = c.scene.addSphereMesh(c.ctx, c.pool, m.sphereRadius, m.name);
    if (idx == std::numeric_limits<std::uint32_t>::max()) {
        Logger::error("SceneLoader: failed to upload sphere '{}'", m.name);
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }
    subs.push_back({idx, ""});
    return subs;

}

// ── Meshes: import each declared mesh ONCE (object space) ─────────
// Records, per declared mesh name, the registered sub-mesh indices (+ OBJ
// group names for material resolution). Instances reference these.
// nullopt = the import arm warned and skipped the mesh.
[[nodiscard]] std::expected<ImportedSubmeshes, VkResult> importMesh(SceneLoadContext& c,
                                                                    const aether::MeshDesc& m) {
    auto ommForGroup = resolveMicromaps(c, m);
    if (!ommForGroup) {
        return std::unexpected(ommForGroup.error());
    }

    std::expected<ImportedSubmeshes, VkResult> arm;
    switch (m.kind) {
    case aether::MeshDesc::Kind::Object:
        arm = importObjectMesh(c, m, *ommForGroup);
        break;
    case aether::MeshDesc::Kind::Box:
        arm = importBoxMesh(c, m);
        break;
    case aether::MeshDesc::Kind::Sphere:
        arm = importSphereMesh(c, m);
        break;
    }
    if (!arm) {
        return std::unexpected(arm.error());
    }
    if (!*arm) {
        return std::nullopt; // import arm warned and skipped
    }
    std::vector<LoadedSubmesh> subs = std::move(**arm);
    if (subs.empty()) {
        Logger::warn("SceneLoader: mesh '{}' produced no geometry — skipping", m.name);
    }
    return subs;
}

// ── Instances: place a mesh with a transform + material ──────────
std::expected<void, VkResult> placeInstances(SceneLoadContext& c, const aether::SceneDesc& desc) {
    // ── Instances: place a mesh with a transform + material ───────────────────
    for (const aether::InstanceDesc& inst : desc.instances) {
        auto it = c.meshSubmeshes.find(inst.meshName);
        if (it == c.meshSubmeshes.end()) {
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

            loadMaterialTextures(c, matName);
            const std::uint32_t matIdx = c.scene.addMaterial(c.lib.getOrDefault(matName));
            if (c.scene.addInstance(sub.meshIndex, xform, matIdx) == std::numeric_limits<std::uint32_t>::max()) {
                Logger::error("SceneLoader: failed to place instance of mesh '{}'", inst.meshName);
                return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
            }
        }
    }

    return {};
}

} // namespace

std::expected<SceneLoader::SceneConfig, VkResult> SceneLoader::load(const std::filesystem::path& sceneFile,
                                                                    const std::filesystem::path& assetsDir,
                                                                    ISceneBuilder& scene,
                                                                    const DeviceContext& ctx,
                                                                    const CommandPool& pool) {
    // Parsing is owned by Aether — SceneLoader only resolves and uploads.
    const std::optional<aether::SceneDesc> desc = aether::SceneParser::parse(sceneFile);
    if (!desc) {
        Logger::error("SceneLoader: cannot open '{}'", sceneFile.string());
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    SceneConfig cfg{};
    parseRenderStageToggles(sceneFile, cfg);
    SceneLoadContext c{assetsDir, ctx, pool, scene, cfg};

    applyWorkingColorSpace(*desc, cfg);
    loadMaterialLibraries(*desc, assetsDir, cfg, c.lib);
    applyRenderSettings(*desc, cfg);
    applyCamera(desc->camera, cfg);
    computeAlphaTested(c, *desc);

    for (const aether::MeshDesc& m : desc->meshes) {
        auto subs = importMesh(c, m);
        if (!subs) {
            return std::unexpected(subs.error());
        }
        if (!*subs || (*subs)->empty()) {
            continue; // importMesh already warned
        }
        c.meshSubmeshes.emplace(m.name, std::move(**subs));
    }

    if (auto placed = placeInstances(c, *desc); !placed) {
        return std::unexpected(placed.error());
    }

    Logger::info("SceneLoader: loaded '{}'", sceneFile.filename().string());
    return cfg;
}

} // namespace harmonia
