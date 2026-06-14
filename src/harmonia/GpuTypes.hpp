#pragma once

#include <volk/volk.h>

#include <cstdint>
#include <type_traits>
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

/// Tone mapper selection (matches PushConstants::tonemapper and tonemap.slang switch).
/// Applied only for SDR and Display P3 output; HDR paths (HDR10/HLG/scRGB) use their
/// own transfer functions and ignore this field.
enum class Tonemapper : uint32_t {
    eACES = 0,     ///< ACES RRT+ODT (Stephen Hill fit)   — filmic, high contrast
    eAgX = 1,      ///< AgX (Troy Sobotka, 2022)          — wide DR, natural highlight rolloff
    eReinhard = 2, ///< Luminance-preserving Reinhard      — simple, smooth, no colour shift
    eHable = 3,    ///< Hable / Uncharted-2 filmic         — moderate contrast, reference
};

/// Scene light types (matches GpuLight::type field and shader constants).
enum class LightType : uint32_t {
    Rect = 0,        ///< Area / rectangular emitter  — intensity in cd/m² (nits)
    Point = 1,       ///< Omnidirectional point light  — intensity in cd or lm
    Spot = 2,        ///< Cone spot light              — intensity in cd or lm
    Directional = 3, ///< Infinitely distant parallel  — intensity in lux
    Sky = 4,         ///< IBL sky dome                 — intensity in cd/m²
};

struct GpuVertex {
    glm::vec3 position;
    float tangentX = 0.0f; ///< Tangent vector X component (world space)
    glm::vec3 normal;
    float tangentY = 0.0f; ///< Tangent vector Y component (world space)
    glm::vec2 uv;
    float tangentZ      = 0.0f; ///< Tangent vector Z component (world space)
    float bitangentSign = 0.0f; ///< ±1 handedness of the bitangent (B = sign × (N × T))
};

struct GpuMaterial {
    glm::vec4 baseColorWeight;
    glm::vec4 baseMetalnessDiffRough;
    glm::vec4 specularColorWeight;
    glm::vec4 specularRoughAnisoIor;
    glm::vec4 transmissionColorWeight;
    glm::vec4 transmissionParams;  ///< x = transmission_depth, y = (spec_roughness dup), z = dispersion_scale, w =
                                   ///< dispersion_abbe_number
    glm::vec4 transmissionScatter; ///< xyz = transmission_scatter (single-scatter albedo), w =
                                   ///< transmission_scatter_anisotropy (g)
    glm::vec4 subsurfaceColorWeight;
    glm::vec4 subsurfaceRadiusScale;
    glm::uvec4 textureIndices; ///< bindless texture indices: [base_color, normal, orm, emission]; ~0u = none
    glm::vec4 thinFilmParams;
    glm::vec4 coatColorWeight;
    glm::vec4 coatRoughAnisoIorDark;
    glm::vec4 fuzzColorWeight;
    glm::vec4 fuzzRoughPad;
    glm::vec4
        emissionColorLum; ///< xyz = emission_color (linear Rec.2020), w = emission_luminance in cd/m² (OpenPBR spec)
    glm::vec4 opacityFlagsPad;  ///< x = geometry_opacity, y = flags, z = subsurface_scatter_anisotropy, w =
                                ///< geometry_thin_walled
    glm::uvec4 textureIndices2; ///< bindless indices: [coat_normal, tangent, coat_tangent, unused]; ~0u = none
};

// GpuInstance is renderer-specific (path-tracer index layout vs rasterizer meshlet
// layout) and is defined by each renderer alongside its own Scene.

/// Per-triangle emissive descriptor for NEE direct area sampling (std430, 64 bytes = 4×float4).
/// Edge vectors and emission components share float4 w-channels to avoid padding.
struct GpuEmissiveTriangle {
    glm::vec4 v0_area;      ///< xyz = v0 world pos, w = triangle area
    glm::vec4 edge1_emitR;  ///< xyz = edge1 (v1-v0) world, w = emission.r
    glm::vec4 edge2_emitG;  ///< xyz = edge2 (v2-v0) world, w = emission.g
    glm::vec4 normal_emitB; ///< xyz = face normal (unit) world, w = emission.b
};

/// GPU-side light descriptor (std430, 64 bytes).
///
/// `intensity` is stored in radiometric units after photometric → radiometric
/// conversion in Light::toGpu() (divides by the luminous efficacy constant 683 lm/W):
///   Rect / Sky   : radiant exitance  [W/sr/m²]  = luminance [cd/m²]    / 683
///   Point / Spot : radiant intensity [W/sr]      = luminous intensity [cd] / 683
///   Directional  : irradiance        [W/m²]      = illuminance [lux]     / 683
struct GpuLight {
    glm::vec3 position;
    float type = 0.0f; ///< reinterpret_cast<uint32_t> → LightType
    glm::vec3 direction;
    float range = 0.0f; ///< attenuation cutoff; 0 = infinite
    glm::vec3 color;
    float intensity  = 0.0f; ///< radiometric (see above)
    float halfWidth  = 0.0f; ///< rect half-width  / spot unused
    float halfHeight = 0.0f; ///< rect half-height / spot unused
    float cosInner   = 0.0f; ///< spot inner cone cos(angle)
    float cosOuter   = 0.0f; ///< spot outer cone cos(angle)
};

struct CameraData {
    glm::mat4 invView;
    glm::mat4 invProj;
    glm::vec4 position;
    float    lensRadius    = 0.0f;
    float    focusDistance = 0.0f;
    uint32_t frameIndex    = 0;
    uint32_t maxDepth      = 0;
    float    exposure      = 0.0f; ///< pre-computed from EV100: 1 / (1.2 * 2^EV100)
    float    _padCam[3]    = {};
};

struct PushConstants {
    uint32_t frameIndex              = 0;
    uint32_t maxDepth                = 0;
    uint32_t rngSeed                 = 0;
    float    envLuminanceScale       = 0.0f;
    uint32_t lightCount              = 0; ///< number of active GpuLights in the light buffer
    uint32_t outputColorSpace        = 0; ///< OutputColorSpace enum value (used by tonemap pass)
    uint32_t samplesPerPixel         = 0; ///< samples per pixel this dispatch
    uint32_t hasEnvMap               = 0; ///< 1 = IBL env map is bound in set1/binding6, 0 = procedural sky
    uint32_t emissiveTriangleCount   = 0; ///< number of emissive triangles for NEE area sampling (0 = disabled)
    uint32_t envImportanceWidth      = 0; ///< CDF grid width for env importance sampling (0 = disabled)
    uint32_t envImportanceHeight     = 0; ///< CDF grid height for env importance sampling
    uint32_t tonemapper              = 0; ///< Tonemapper enum value; SDR/P3 only (0 = eACES)
    uint32_t workingColorSpace       = 0; ///< ColorSpace::WorkingColorSpace (0 = lin Rec.2020, 1 = lin Rec.709)
};

/// TLAS instance mask bit used in TraceRay InstanceInclusionMask comparisons.
/// All instances use the default mask (all-bits-set); no per-type masking is needed
/// since shadow rays stop just before the emissive surface via a calibrated tMax.
static constexpr uint32_t kInstanceMaskAll = 0xFFU; ///< all instances visible

static_assert(std::is_trivially_copyable_v<GpuVertex>);

/// Sentinel texture index: slot holds no texture.
static constexpr uint32_t kNoTexture = ~0u;
static_assert(std::is_trivially_copyable_v<GpuMaterial>);
static_assert(std::is_trivially_copyable_v<GpuLight>);
static_assert(std::is_trivially_copyable_v<GpuEmissiveTriangle>);
static_assert(std::is_trivially_copyable_v<CameraData>);
static_assert(std::is_trivially_copyable_v<PushConstants>);

static_assert(sizeof(GpuVertex) == 48);
static_assert(sizeof(GpuMaterial) == 288);
static_assert(sizeof(GpuLight) == 64);
static_assert(sizeof(GpuEmissiveTriangle) == 64);
static_assert(sizeof(CameraData) == 176);
static_assert(sizeof(PushConstants) == 52);
