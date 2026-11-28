/// gpu_types.slang — shared GPU struct layouts and constants.
///
/// Imported by both Hyperion (path tracer) and Theia (real-time renderer)
/// to guarantee a single authoritative definition of each struct.
/// Renderer-specific types (GpuInstance, GpuMeshlet, ray-payload, bindings)
/// stay in each renderer's own common.slang.

struct GpuVertex {
    float3 position;
    float  tangentX;    ///< Tangent vector X component (world space)
    float3 normal;
    float  tangentY;    ///< Tangent vector Y component (world space)
    float2 uv;
    float  tangentZ;    ///< Tangent vector Z component (world space)
    float  bitangentSign; ///< ±1 handedness: B = sign × (N × T)
};

struct GpuMaterial {
    float4 baseColorWeight;
    float4 baseMetalnessDiffRough; // x=metalness, y=diffuse roughness, z=textured-emission hit-only ownership
    float4 specularColorWeight;
    float4 specularRoughAnisoIor;
    float4 transmissionColorWeight;
    float4 transmissionParams;     ///< x = transmission_depth, z = dispersion_scale, w = dispersion_abbe_number
    float4 transmissionScatter;    ///< xyz = transmission_scatter albedo, w = transmission_scatter_anisotropy (g)
    float4 subsurfaceColorWeight;
    float4 subsurfaceRadiusScale;
    uint4  textureIndices;  ///< bindless indices: [base_color, normal, orm, emission]; ~0u = none
    float4 thinFilmParams;
    float4 coatColorWeight;
    float4 coatRoughAnisoIorDark;
    float4 fuzzColorWeight;
    float4 fuzzRoughPad;
    float4 emissionColorLum;
    float4 opacityFlagsPad;  ///< x = opacity, y = flags, z = subsurface_scatter_anisotropy, w = thin_walled
    uint4  textureIndices2;  ///< bindless indices: [coat_normal, tangent, coat_tangent, unused]; ~0u = none
    uint4  textureIndices3;  ///< [coat_weight, transmission_weight, subsurface_color, subsurface_weight]
    uint4  textureIndices4;  ///< [transmission_color, transmission_scatter, emission_luminance, fuzz_weight]
};

/// Tagged-union emissive descriptor for NEE direct sampling (std430, 80 bytes = 5×float4).
/// Edge vectors and emission components share float4 w-channels to avoid padding.
/// kind_pad.x selects the branch (kEmissiveTriangle / kEmissiveSphere): triangles use
/// v0/edge1/edge2/normal; spheres use the world-space center (v0_area.xyz) + radius
/// (v0_area.w) and reuse only the emission w-channels (edge1/edge2/normal xyz unused).
struct GpuEmissiveTriangle {
    float4 v0_area;      ///< tri: xyz=v0, w=area | sphere: xyz=center, w=radius
    float4 edge1_emitR;  ///< tri: xyz=edge1, w=emission.r | sphere: w=emission.r
    float4 edge2_emitG;  ///< tri: xyz=edge2, w=emission.g | sphere: w=emission.g
    float4 normal_emitB; ///< tri: xyz=face normal, w=emission.b | sphere: w=emission.b
    uint4  kind_pad;     ///< kind, material, instance, primitive (collectable light ownership)
};

/// Emissive light kind constants — match the EmissiveKind C++ enum in GpuTypes.hpp.
static const uint kEmissiveTriangle = 0u;
static const uint kEmissiveSphere   = 1u;

/// GPU light descriptor.  intensity is the light's photometric value, unchanged from
/// Light::toGpu(): rect / sky luminance [cd/m²], point / spot luminous intensity [cd],
/// directional illuminance [lux] - the same scale as emissive materials and the environment.
struct GpuLight {
    float3 position;  float  typeF;     ///< asuint(typeF) == LightType constant
    float3 direction; float  range;     ///< attenuation cutoff; 0 = infinite
    float3 color;     float  intensity; ///< photometric (cd/m², cd, or lux)
    float  halfWidth;                   ///< rect: half extent along the rolled in-plane axis
    float  halfHeight;                  ///< rect: half extent along the perpendicular in-plane axis
    float  cosInner;                    ///< spot: cos(inner half angle); rect: roll about the normal [rad]
    float  cosOuter;                    ///< spot: cos(outer half angle)
};

/// Light type constants — match the LightType C++ enum in GpuTypes.hpp.
static const uint kLightRect        = 0;
static const uint kLightPoint       = 1;
static const uint kLightSpot        = 2;
static const uint kLightDirectional = 3;
static const uint kLightSky         = 4;

/// Mathematical constants.
static const float kPi       = 3.14159265359;
static const float kInvPi    = 0.31830988618;
static const float kHalfPi   = 1.57079632679; ///< π/2

/// Ray-tracing epsilon for surface self-intersection avoidance.
static const float kRayEpsilon = 0.001;

/// Epsilon for volumetric random-walk segments (much smaller than kRayEpsilon: dense media have
/// free-flight distances below kRayEpsilon). Shared by both renderers' medium walk.
static const float kMediumEpsilon = 1.0e-5;

/// Epsilon for resuming a ray after a NULL interaction — the OpenPBR `geometry_opacity`
/// pass-through, where the ambient-medium side of mix(ambient, surface, alpha) was selected.
/// Deliberately far below kRayEpsilon: nothing is scattered off a shading normal here, the
/// SAME ray continues, so the only thing to step over is the surface just crossed. Using the
/// scattering epsilon would make the continuation skip every surface within 1 mm of the
/// cut-out — dropping the ray straight through coincident geometry (a cut-out object resting
/// on a floor lands *inside* the floor) and rendering the hole black.
static const float kNullInteractionEpsilon = 1.0e-5;

/// Effective infinity for ray TMax / hit-distance comparisons.
static const float kInf = 1.0e30;

/// Minimum path depth before Russian Roulette path termination is applied.
static const uint kRRMinDepth = 3u;

/// Sentinel: a material texture slot holds no texture (matches the host-side `kNoTexture`).
static const uint kNoTexture = 0xFFFFFFFFu;

/// Rec.2020 luminance coefficients (ITU-R BT.2020).
static const float3 kRec2020Luma = float3(0.2627, 0.6780, 0.0593);

/// TLAS instance mask — all instances visible.
static const uint kInstanceMaskAll = 0xFFu;
