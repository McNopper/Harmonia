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
    float4 baseMetalnessDiffRough;
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
};

/// Per-triangle emissive descriptor for NEE direct area sampling (std430, 64 bytes = 4×float4).
/// Edge vectors and emission components share float4 w-channels to avoid padding.
struct GpuEmissiveTriangle {
    float4 v0_area;      ///< xyz = v0 world pos, w = triangle area
    float4 edge1_emitR;  ///< xyz = edge1 (v1-v0) world, w = emission.r
    float4 edge2_emitG;  ///< xyz = edge2 (v2-v0) world, w = emission.g
    float4 normal_emitB; ///< xyz = face normal (unit) world, w = emission.b
};

/// GPU light descriptor.  intensity is in radiometric units (see Light.cpp toGpu()).
struct GpuLight {
    float3 position;  float  typeF;     ///< asuint(typeF) == LightType constant
    float3 direction; float  range;     ///< attenuation cutoff; 0 = infinite
    float3 color;     float  intensity; ///< radiometric (W/sr, W/sr/m², or W/m²)
    float  halfWidth;
    float  halfHeight;
    float  cosInner;
    float  cosOuter;
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

/// Effective infinity for ray TMax / hit-distance comparisons.
static const float kInf = 1.0e30;

/// Minimum path depth before Russian Roulette path termination is applied.
static const uint kRRMinDepth = 3u;

/// Rec.2020 luminance coefficients (ITU-R BT.2020).
static const float3 kRec2020Luma = float3(0.2627, 0.6780, 0.0593);

/// TLAS instance mask — all instances visible.
static const uint kInstanceMaskAll = 0xFFu;
