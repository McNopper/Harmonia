// Standalone bindings for the tone-mapping fragment pass.
// We do NOT import common.slang to avoid pulling in RaytracingAccelerationStructure
// (only valid in ray-tracing stages) and other ray-tracing-specific resources.

import aces;

[[vk::binding(1, 0)]] RWTexture2D<float4> hdrTarget;
/// The glare of the frame (the GlarePass's result, at half resolution): the
/// light a lens scatters from the bright parts of the frame. A null descriptor
/// (no glare) reads as zero. A 32-bit float image, for which linear filtering
/// is not a guaranteed format feature: the draw interpolates it (glareAt).
[[vk::binding(2, 0)]] Texture2D<float4> glareImage;

/// The block the host pushes (GpuTypes.hpp::PushConstants), member by member:
/// the host pushes outputColorSpace, tonemapper, workingColorSpace and
/// glareStrength each at the offset of its C++ member.
struct PushConstants {
    uint  frameIndex;
    uint  maxDepth;
    uint  rngSeed;
    float envLuminanceScale;
    uint  lightCount;
    uint  analyticLightCount;    ///< analytic prefix, before synthesized emissive lights
    uint  outputColorSpace;      ///< OutputColorSpace enum (used by tonemap pass)
    uint  samplesPerPixel;
    uint  hasEnvMap;
    uint  emissiveTriangleCount;
    uint  envImportanceWidth;
    uint  envImportanceHeight;
    uint  tonemapper;            ///< Tonemapper enum: 0=ACES, 1=AgX, 2=Reinhard, 3=Hable
    uint  workingColorSpace;     ///< 0 = linear Rec.2020 (default), 1 = linear Rec.709
    // Not read by the tone mapper; declared so that glareStrength has the
    // offset of its C++ member. smsWork is a VkDeviceAddress: 8 bytes.
    uint  smsPartitionResolution;
    uint2 smsWork;
    float hazeDensity;
    float hazeHeight;
    float glareStrength;         ///< share of the glare texture added to the frame (0 = none)
};
[[vk::push_constant]] PushConstants pushConst;

// ---------------------------------------------------------------------------
// Color space conversion matrices (working space: linear Rec.2020)
// ---------------------------------------------------------------------------

/// Rec.2020 → Rec.709 (D65)
/// Coefficients from ITU-R BT.2087 / IEC 61966-2-1.
float3 rec2020ToRec709(float3 c) {
    return float3(
        dot(c, float3( 1.6605f, -0.5876f, -0.0728f)),
        dot(c, float3(-0.1246f,  1.1329f, -0.0083f)),
        dot(c, float3(-0.0182f, -0.1006f,  1.1187f))
    );
}

/// Rec.709 → Rec.2020 (D65)
/// Inverse of the above; used to up-convert a Rec.709 working space.
float3 rec709ToRec2020(float3 c) {
    return float3(
        dot(c, float3( 0.6274040f,  0.3292820f,  0.0433140f)),
        dot(c, float3( 0.0690970f,  0.9195400f,  0.0113630f)),
        dot(c, float3( 0.0163916f,  0.0880132f,  0.8955950f))
    );
}

/// ACEScg AP1 D60 → Rec.2020 D65 (Bradford adapted).
float3 ap1ToRec2020(float3 c) {
    const float3x3 m = float3x3(
        float3( 1.0258247477f, -0.0200531908f, -0.0057715568f),
        float3(-0.0022343695f,  1.0045865019f, -0.0023521324f),
        float3(-0.0050133515f, -0.0252900718f,  1.0303034233f));
    return mul(m, c);
}

/// Rec.2020 → Display P3 D65
float3 rec2020ToP3(float3 c) {
    return float3(
        dot(c, float3( 1.2249f, -0.2247f,  0.0000f)),
        dot(c, float3(-0.0420f,  1.0419f,  0.0000f)),
        dot(c, float3( 0.0000f, -0.0197f,  1.0197f))
    );
}

/// Rec.709 D65 → Display P3 D65
/// Used when a non-ACES tone mapper produces Rec.709 output for P3 display.
static const float3x3 kRec709ToP3 = float3x3(
    float3(0.822462f, 0.177538f, 0.000000f),
    float3(0.033194f, 0.966806f, 0.000000f),
    float3(0.017083f, 0.072340f, 0.910577f)
);

// ---------------------------------------------------------------------------
// ACES AP1 (ACEScg) tone mapping
// ---------------------------------------------------------------------------
//
// Colour matrices derived from the ACES specification S-2014-003 published by
// the Academy of Motion Picture Arts and Sciences (AMPAS).  The RRT+ODT
// analytic fit is by Stephen Hill (the BakingLab ACES.hlsl):
//   https://github.com/TheRealMJP/BakingLab/blob/master/BakingLab/ACES.hlsl
// The rational polynomial coefficients are a mathematical description of the
// tone curve; no copyrightable expression is reproduced.
//
// All matrices are row-major for Slang float3x3 (mul(M, v) = M·v).
// These hold the same numeric values as src/harmonia/utils/ColorSpace.cpp.

/// Rec.2020 → ACEScg AP1
static const float3x3 kRec2020ToAP1 = float3x3(
    float3(0.6131324f,  0.3395255f,  0.0474491f),  // AP1.R
    float3(0.0701243f,  0.9163394f,  0.0135363f),  // AP1.G
    float3(0.0205076f,  0.1096098f,  0.8699926f)   // AP1.B
);

/// ACEScg AP1 → Rec.709 D65  (composed: AP1 → Rec.2020 → Rec.709)
static const float3x3 kAP1ToRec709 = float3x3(
    float3( 2.9090f, -1.6933f, -0.2159f),  // Rec.709.R
    float3(-0.3595f,  1.3712f, -0.0114f),  // Rec.709.G
    float3(-0.0447f, -0.2477f,  1.2925f)   // Rec.709.B
);

/// ACEScg AP1 → Display P3 D65  (composed: AP1 → Rec.2020 → P3)
static const float3x3 kAP1ToP3 = float3x3(
    float3( 2.1173f, -1.0180f, -0.0996f),  // P3.R
    float3(-0.2071f,  1.2150f, -0.0077f),  // P3.G
    float3(-0.0219f, -0.1540f,  1.1758f)   // P3.B
);

/// ACES RRT+ODT rational-polynomial approximation (Stephen Hill, 2016).
/// Input/output are ACEScg AP1 linear.
float3 acesRrtOdtFit(float3 v) {
    float3 a = v * (v + 0.0245786f) - 0.000090537f;
    float3 b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
    return a / b;
}

/// Full ACES tone mapper: Rec.2020 linear → tone-mapped output in target primaries.
/// outputMat must be kAP1ToRec709 (for SDR / scRGB) or kAP1ToP3 (for Display P3).
float3 acesFitted(float3 c2020, float3x3 outputMat) {
    float3 ap1 = mul(kRec2020ToAP1, c2020);
    ap1 = max(ap1, 0.0f.xxx);        // clamp negatives before tone curve
    ap1 = acesRrtOdtFit(ap1);
    return max(mul(outputMat, ap1), 0.0f.xxx);
}

// ---------------------------------------------------------------------------
// Legacy / fallback tone mappers
// ---------------------------------------------------------------------------

/// Hable / Uncharted-2 filmic (John Hable, GDC 2010).
/// Kept for reference; ACES is preferred for new content.
float3 hablePartial(float3 x) {
    const float A = 0.15f, B = 0.50f, C = 0.10f, D = 0.20f, E = 0.02f, F = 0.30f;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}
float3 hableFilmic(float3 x) {
    const float3 white = hablePartial(float3(11.2f, 11.2f, 11.2f));
    return hablePartial(2.0f * x) / white;
}

/// Luminance-preserving Reinhard (Reinhard et al., "Photographic Tone
/// Reproduction for Digital Images", SIGGRAPH 2002).
/// Tone-maps luminance, preserves Rec.2020 chromaticity.
float3 reinhardLuminance(float3 x) {
    const float lum = dot(x, float3(0.2627f, 0.6780f, 0.0593f)); // Rec.2020 Kr/Kg/Kb
    if (lum <= 0.0f) return 0.0f.xxx;
    const float lumTm = lum / (1.0f + lum);
    return x * (lumTm / lum);
}

// ---------------------------------------------------------------------------
// AgX tone mapping
// ---------------------------------------------------------------------------
//
// AgX is an open tone-mapping transform by Troy Sobotka (2022), used in
// Blender and the OCIO reference configuration.  It targets wide-dynamic-range
// scenes (outdoor IBL, direct sun) and produces smooth, desaturated highlights
// instead of ACES' hard chromatic shoulder.
//
// Pipeline (all values in Rec.709 / sRGB primaries):
//   1. Rec.2020 → Rec.709 linear
//   2. Gamut inset  (mild desaturation for display-gamut safety)
//   3. Log₂ encode: (log₂(max(v, 2.2e−7)) + 10) / 16.5  →  [0, 1]
//      Working EV range: [−10, +6.5]  ≈ [0.001 nit, ~90 nit at reference white]
//   4. Per-channel S-curve polynomial
//   5. Gamut outset (undo inset, linear Rec.709 output)
//   6. Caller applies sRGB OETF
//
// S-curve:  15.5x⁶ − 40.14x⁵ + 31.96x⁴ − 6.868x³ + 0.4298x² + 0.1191x − 0.00232
//
// Matrices from Blender / OCIO AgX config (MIT license, Troy Sobotka):
//   https://github.com/blender/blender/blob/main/release/datafiles/colormanagement/config.ocio

/// AgX input (inset) matrix — mild gamut compression toward the display gamut.
static const float3x3 kAgXInputMatrix = float3x3(
    float3(0.842479f, 0.042328f, 0.042376f),
    float3(0.078434f, 0.878469f, 0.078434f),
    float3(0.079224f, 0.079166f, 0.879143f)
);

/// AgX output (outset) matrix — approximate inverse of the inset.
static const float3x3 kAgXOutputMatrix = float3x3(
    float3( 1.196879f, -0.052897f, -0.052972f),
    float3(-0.098021f,  1.151903f, -0.098043f),
    float3(-0.099030f, -0.098961f,  1.151074f)
);

/// Per-channel AgX S-curve polynomial.
float agxCurve(float x) {
    x = saturate(x);
    const float x2 = x  * x;
    const float x3 = x2 * x;
    const float x4 = x2 * x2;
    const float x5 = x3 * x2;
    const float x6 = x3 * x3;
    return 15.5f*x6 - 40.14f*x5 + 31.96f*x4 - 6.868f*x3 + 0.4298f*x2 + 0.1191f*x - 0.00232f;
}
float3 agxCurve3(float3 v) { return float3(agxCurve(v.x), agxCurve(v.y), agxCurve(v.z)); }

/// Full AgX tone mapper: Rec.2020 linear → Rec.709 linear (pre-OETF).
/// Caller must apply sRGB OETF and clamp to [0, 1].
float3 agxFitted(float3 c2020) {
    float3 agx = max(rec2020ToRec709(c2020), 0.0f.xxx);
    agx = mul(kAgXInputMatrix, agx);
    agx = (log2(max(agx, 2.2e-7f.xxx)) + 10.0f) / 16.5f;
    agx = agxCurve3(agx);
    return mul(kAgXOutputMatrix, agx);
}

// ---------------------------------------------------------------------------
// Tonemapper dispatch
// ---------------------------------------------------------------------------

/// Apply the selected tone mapper (pushConst.tonemapper) to HDR Rec.2020 input.
/// Returns Rec.709 linear — caller must apply OETF and final clamp.
float3 dispatchTonemapper(float3 hdr, uint tm) {
    switch (tm) {
        case 1:  return agxFitted(hdr);                                           // AgX
        case 2:  return saturate(reinhardLuminance(rec2020ToRec709(hdr)));        // Reinhard
        case 3:  return saturate(hableFilmic(rec2020ToRec709(hdr)));              // Hable
        default: return acesFitted(hdr, kAP1ToRec709);                           // ACES (0)
    }
}

// ---------------------------------------------------------------------------
// Transfer functions (OETF)
// ---------------------------------------------------------------------------

/// SMPTE ST.2084 PQ OETF — input in absolute nits, output in [0, 1].
/// 1.0 = 10 000 nit peak (HDR10 maximum).
float pqOetf(float L) {
    const float m1 = 0.1593017578125f;
    const float m2 = 78.84375f;
    const float c1 = 0.8359375f;
    const float c2 = 18.8515625f;
    const float c3 = 18.6875f;
    float Yn  = saturate(L / 10000.0f);
    float Ym1 = pow(Yn, m1);
    return pow((c1 + c2 * Ym1) / (1.0f + c3 * Ym1), m2);
}
float3 pqOetf3(float3 c) { return float3(pqOetf(c.x), pqOetf(c.y), pqOetf(c.z)); }

/// ITU-R BT.2100 HLG OETF.
/// Input: scene-linear, 1.0 = HLG reference white (BT.2100 scene-referred).
/// Output in [0, 1]; 0.75 corresponds to reference white.
/// Constants: a = 0.17883277, b = 1 - 4a, c = 0.5 - a·ln(4a)  (BT.2100 Table 5).
float hlgOetf(float E) {
    const float a     = 0.17883277f;
    const float b     = 0.28466892f;
    const float c_hlg = 0.55991073f;
    E = max(0.0f, E);
    return (E <= (1.0f / 12.0f))
        ? sqrt(3.0f * E)
        : (a * log(12.0f * E - b) + c_hlg);
}
float3 hlgOetf3(float3 c) { return float3(hlgOetf(c.x), hlgOetf(c.y), hlgOetf(c.z)); }

/// sRGB / BT.709 OETF (IEC 61966-2-1).
float srgbOetf(float x) {
    x = max(0.0f, x);
    return (x <= 0.0031308f) ? (12.92f * x) : (1.055f * pow(x, 1.0f / 2.4f) - 0.055f);
}
float3 srgbOetf3(float3 c) { return float3(srgbOetf(c.x), srgbOetf(c.y), srgbOetf(c.z)); }

// Display P3 uses the same OETF curve as sRGB (IEC 61966-2-1).
float3 p3Oetf3(float3 c) { return srgbOetf3(c); }

// ---------------------------------------------------------------------------
// HDR10 highlight roll-off
// ---------------------------------------------------------------------------
//
// Scene-linear 1.0 maps to the ITU-R BT.2408-5 (2022) reference white of
// 203 nit.  A Reinhard-style luminance shoulder prevents hard clipping at the
// display peak (typically 1 000 nit for consumer HDR10 displays).

static const float kHDR10PaperWhiteNits = 203.0f;  // ITU-R BT.2408-5 reference white
static const float kHDR10KneeNits       = 600.0f;  // begin roll-off
static const float kHDR10PeakNits       = 1000.0f; // target display peak

/// Soft-clips per-channel luminance to kHDR10PeakNits via a Reinhard shoulder.
/// Preserves chromaticity by scaling uniformly by the luminance ratio.
float3 hdr10SoftClip(float3 nitsLinear) {
    const float lum = dot(nitsLinear, float3(0.2627f, 0.6780f, 0.0593f));
    if (lum <= 0.0f) return 0.0f.xxx;
    float lumOut;
    if (lum <= kHDR10KneeNits) {
        lumOut = lum;
    } else {
        // Reinhard shoulder mapped to [knee, peak]
        float t = (lum - kHDR10KneeNits) / (kHDR10PeakNits - kHDR10KneeNits);
        lumOut  = kHDR10KneeNits + (kHDR10PeakNits - kHDR10KneeNits) * t / (1.0f + t);
    }
    return nitsLinear * (lumOut / lum);
}

// ---------------------------------------------------------------------------
// Firefly suppression (Karis / SMAA-inspired neighbourhood soft-clamp)
// ---------------------------------------------------------------------------
//
// Rare spectral or SSS fireflies produce isolated pixels that are many times
// brighter than their neighbours. Left unclamped they can contaminate
// auto-exposure, bloom, or temporal accumulation.
//
// Algorithm: read the 3×3 neighbourhood from hdrTarget and compute the
// maximum neighbour luminance. If the centre pixel exceeds 4× that maximum
// (and is above the fast-path threshold of 10 scene-linear units), scale it
// back to 4× the neighbour max. The 4× headroom allows real bright pixels
// (e.g. a specular highlight near a dark background) to survive; only true
// isolated outliers are attenuated.
//
// This is a display-stage-only operation: it does NOT modify the accumulation
// buffer, so the path-integrator estimator remains unbiased.

/// Soft-clamp an HDR centre pixel against its 3×3 neighbourhood maximum.
/// Uses Rec.2020 luminance coefficients for both working spaces; the slight
/// inaccuracy for Rec.709 inputs is negligible given the rough threshold.
float3 softenFirefly(RWTexture2D<float4> hdr, int2 coord, float3 center) {
    const float3 kLumWeights = float3(0.2627f, 0.6780f, 0.0593f); // Rec.2020 Kr/Kg/Kb
    float centerLum = dot(center, kLumWeights);
    if (centerLum < 10.0f) return center; // fast path: well below any firefly threshold

    float maxNeighbourLum = 0.0f;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) continue;
            float3 n = max(hdr[coord + int2(dx, dy)].xyz, 0.0f.xxx);
            maxNeighbourLum = max(maxNeighbourLum, dot(n, kLumWeights));
        }
    }

    // Allow up to 4× the brightest neighbour; clamp floor of 10.0 ensures
    // we never suppress a uniformly-bright region below threshold.
    float clampLum = max(maxNeighbourLum * 4.0f, 10.0f);
    if (centerLum > clampLum) {
        center *= clampLum / centerLum;
    }
    return center;
}

// ---------------------------------------------------------------------------
// Lens glare
// ---------------------------------------------------------------------------

/// The glare under bilinear filtering at the frame position uv: the half
/// resolution texels around it, weighted as a linear filter does.
float3 glareAt(float2 uv) {
    uint width = 0u;
    uint height = 0u;
    glareImage.GetDimensions(width, height);
    const float2 size = float2(width, height);
    const float2 position = uv * size - 0.5;
    const int2 base = int2(floor(position));
    const float2 offset = position - float2(base);
    const int2 last = int2(size) - int2(1, 1);
    float4 sum = 0.0.xxxx;
    [unroll]
    for (uint dy = 0u; dy < 2u; ++dy) {
        const float wy = (dy == 0u) ? (1.0f - offset.y) : offset.y;
        [unroll]
        for (uint dx = 0u; dx < 2u; ++dx) {
            const float wx = (dx == 0u) ? (1.0f - offset.x) : offset.x;
            sum += wx * wy * glareImage.Load(int3(clamp(base + int2(dx, dy), int2(0, 0), last), 0));
        }
    }
    return sum.xyz;
}

// ---------------------------------------------------------------------------
// Fragment entry point
// ---------------------------------------------------------------------------

[shader("fragment")]
float4 tonemapFrag(float4 svPos : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    int2 coord = int2(svPos.xy);
    if (pushConst.tonemapper == 4u && pushConst.outputColorSpace >= 3u) {
        // Full ACES SDR Video: no early scene clamp before the RRT sweeteners.
        float3 fullInput = softenFirefly(hdrTarget, coord, hdrTarget[coord].xyz);
        if (pushConst.glareStrength > 0.0f) {
            fullInput += pushConst.glareStrength * max(glareAt(uv), 0.0f.xxx);
        }
        if (pushConst.outputColorSpace == 5u) {
            return float4(aces10SdrVideoToSrgbFromWorking(fullInput, pushConst.workingColorSpace), 1.0f);
        }
        float3 fullLinear = aces10SdrVideoToLinearFromWorking(fullInput, pushConst.workingColorSpace);
        if (pushConst.outputColorSpace == 4u) {
            return float4(fullLinear, 1.0f);
        }
        return float4(p3Oetf3(saturate(mul(kRec709ToP3, fullLinear))), 1.0f);
    }
    float3 hdr = max(hdrTarget[coord].xyz, 0.0f.xxx);
    hdr = softenFirefly(hdrTarget, coord, hdr); // suppress isolated HDR outliers (fireflies)

    // Lens glare: the light the lens scatters from the bright parts of the frame
    // is added before the tone curve. A presentation stage, in the frame's
    // working space like the frame itself.
    if (pushConst.glareStrength > 0.0f) {
        hdr += pushConst.glareStrength * max(glareAt(uv), 0.0f.xxx);
    }

    // The ToneMapper is the glue between the scene-referred working space and
    // the display-referred output space. The paths below assume linear
    // Rec.2020 input; a Rec.709 working space is up-converted here first
    // (exact — Rec.709 is a subset of Rec.2020).
    if (pushConst.workingColorSpace == 1u) {
        hdr = rec709ToRec2020(hdr);
    } else if (pushConst.workingColorSpace == 2u) {
        hdr = ap1ToRec2020(hdr);
    }
    float3 out3;

    // pushConst.outputColorSpace values (must match OutputColorSpace enum in GpuTypes.hpp):
    //   0 = HDR10     — PQ OETF (SMPTE ST.2084), BT.2020 primaries
    //   1 = HLG       — HLG OETF (ITU-R BT.2100), BT.2020 primaries
    //   2 = scRGB     — extended linear Rec.709; compositor applies display EOTF
    //   3 = Display P3 — ACES filmic, P3 D65 primaries, sRGB-like OETF
    //   4 = SDR linear — Rec.709, clamped, no OETF (debug/capture)
    //   5 = SDR       — ACES filmic, Rec.709 primaries, sRGB OETF  (default / fallback)
    switch (pushConst.outputColorSpace) {
        case 0: {
            // HDR10: scale scene-linear to nits, soft-clip highlight, apply PQ OETF.
            // Paper white 203 nit per ITU-R BT.2408-5; peak 1 000 nit for typical displays.
            float3 nits = hdr * kHDR10PaperWhiteNits;
            nits = hdr10SoftClip(nits);
            out3 = pqOetf3(nits);
            break;
        }
        case 1: {
            // HLG: scene-linear is already normalized to reference white = 1.0.
            // Apply HLG OETF directly (ITU-R BT.2100 scene-referred usage).
            out3 = saturate(hlgOetf3(hdr));
            break;
        }
        case 2: {
            // scRGB: Rec.2020 → Rec.709, output linear (extended values > 1 allowed).
            // The OS compositor / display driver applies the display EOTF.
            out3 = rec2020ToRec709(hdr);
            break;
        }
        case 3: {
            // Display P3: tone map then apply P3 primaries and P3 OETF.
            // ACES (tm=0): use dedicated AP1→P3 matrix for optimal gamut coverage.
            // Others:      tone map to Rec.709 then lift to P3 D65 primaries.
            float3 p3;
            if (pushConst.tonemapper == 0u) {
                p3 = acesFitted(hdr, kAP1ToP3);
            } else {
                p3 = mul(kRec709ToP3, dispatchTonemapper(hdr, pushConst.tonemapper));
            }
            out3 = p3Oetf3(saturate(p3));
            break;
        }
        case 4: {
            // SDR linear: Rec.2020 → Rec.709, clamped, no OETF.
            out3 = saturate(rec2020ToRec709(hdr));
            break;
        }
        default: {
            // SDR sRGB: tone map to Rec.709, then sRGB OETF.
            out3 = srgbOetf3(saturate(dispatchTonemapper(hdr, pushConst.tonemapper)));
            break;
        }
    }

    return float4(out3, 1.0f);
}
