// path_integrator.slang — shared unidirectional path-integrator surface estimator.
//
// The per-surface Monte-Carlo estimator (emission guard + stochastic opacity gate +
// next-event estimation for emissive triangles and the environment + BSDF continuation
// + Russian roulette) shared 1:1 between Hyperion (path tracer, ground truth) and
// Theia (real-time GI compute stage).
//
// Renderer-specific pieces stay out of this module:
//   • Shadow-ray tracing is abstracted behind ITracer (Hyperion: TraceRay miss shader,
//     Theia: inline RayQuery), so the identical estimator drives both.
//   • The BSDF model (evalBSDF / pdfBSDF / sampleBSDF) is supplied by the renderer's own
//     `bsdf` module and resolved at link time — only forward declarations live here, so
//     this module never couples to a specific BSDF implementation.
//   • The environment map / sampler and all scene buffers are passed as explicit
//     parameters rather than read from globals, keeping the module renderer-agnostic.
//
// Ref: Shirley, Wang & Zimmerman — "Monte Carlo Techniques for Direct Lighting
//        Calculations" (ACM TOG 1996) — emissive-triangle area sampling & area→solid-angle PDF
//      Veach — "Robust Monte Carlo Methods for Light Transport Simulation" §9.2 — balance heuristic MIS
//      Pharr, Jakob & Humphreys — "Physically Based Rendering" 4th ed. §13.4.3 (MIS direct lighting),
//        §12.5 (separable-CDF infinite area lights) — pbrt.org

import common;       // GpuMaterial, GpuEmissiveTriangle, kRRMinDepth, kRayEpsilon, kInf, kInvPi
import math;         // randFloat
import bsdf_shared;  // BSDFSample
import env_sample;   // sampleEnvImportanceDir, evalEnvImportancePdfDir, sanitizePdf, balanceHeuristic
// Renderer-supplied BSDF model (evalBSDF / pdfBSDF / sampleBSDF). The `bsdf` module is
// resolved from the consuming renderer's own shader tree at link time (Hyperion and Theia
// each provide their own bsdf.slang), so this estimator stays BSDF-implementation agnostic.
import bsdf;

/// Shadow-tracing abstraction: return true if origin→direction*tMax is UNOCCLUDED.
/// Hyperion: implements with TraceRay (shadow miss shader sets inShadow=false).
/// Theia:    implements with inline RayQuery<ACCEPT_FIRST_HIT | SKIP_PROCEDURAL>.
interface ITracer {
    bool traceOccluded(float3 origin, float3 direction, float tMax);
}

/// Pre-resolved per-bounce hit data. The caller applies texture maps and flips the
/// shading/geometric normals to face `wo` before populating this struct.
struct SurfaceHit {
    float3   position;
    float3   normal;    ///< shading normal flipped to face wo
    float3   geoNormal; ///< geometric normal flipped to face wo
    float3   T, B;      ///< shading tangent frame
    float3   coatN;
    float3   coatT, coatB;
    GpuMaterial mat;    ///< material with texture maps pre-applied
    float3   wo;        ///< outgoing direction (toward camera/previous vertex)
};

/// Mutable path-walk state. `nextOrigin`/`nextDirection` are written by
/// integrateSurface and read back by the caller to spawn the continuation ray.
struct PathState {
    float3  radiance;
    float3  throughput;
    uint    depth;
    uint    seed;
    bool    terminated;
    bool    specularBounce;
    float   lastBsdfPdf;
    float3  nextOrigin;     ///< written by integrateSurface; read by caller for next bounce
    float3  nextDirection;
    // Step 3 (bulk subsurface random walk): set true when this bounce enters a bulk
    // subsurface medium (BSDFSample.entersMedium). Renderers that implement the walk (see
    // Hyperion's raygen/miss/closesthit) read these back from the caller's payload and drive
    // their own volumetric loop; renderers that don't (Theia, for now) simply ignore them —
    // sampleBSDF never sets entersMedium=true there, so these stay false/0.
    bool    insideMedium;
    float   mediumSigmaT;
    float3  mediumAlbedo;
    float   mediumG;
    float   mediumEta;
};

/// Equirectangular (lat-long) env-map lookup matching env.slang's sampleEnvMap convention.
/// Passed-in texture/sampler keep this module free of renderer globals.
float3 sampleEnvMapLatLong(Texture2D<float4> envMap, SamplerState envSampler, float3 dir)
{
    return max(envMap.SampleLevel(envSampler, dirToLatlong(dir), 0).xyz, 0.0.xxx);
}

/// Detailed environment NEE sample payload so callers can reuse the exact shared
/// measure/PDF/MIS conventions while also accessing the sampled direction/pdf.
struct EnvNeeSample {
    float3 wi;
    float  envPdf;
    float  bsdfPdf;
    float  misWeight;
    float3 contribution; // throughput-free radiance contribution
    bool   valid;
};

/// Emission contribution for a directly-hit surface, guarded against MIS double counting.
/// Returns 0 unless the surface is an emitter seen on its front side AND the emission may
/// be added on the camera path: when no NEE is active (emissiveTriangleCount==0), for the
/// primary camera ray (depth==0), or right after a specular/delta bounce (NEE cannot sample
/// through a perfect mirror). All other direct emitter hits are handled by NEE below.
float3 evalEmissionGuarded(float3 emission, uint depth, bool specularBounce,
    float3 wo, float3 geoNormal, uint emissiveTriangleCount)
{
    if (max(emission.x, max(emission.y, emission.z)) <= 0.0 || dot(wo, geoNormal) <= 0.0) {
        return 0.0.xxx;
    }
    if (emissiveTriangleCount == 0u || depth == 0u || specularBounce) {
        return emission;
    }
    return 0.0.xxx;
}

/// Next-event estimation against the emissive-triangle set via direct area sampling.
/// Uniform triangle selection, Shirley sqrt-folding barycentric sample, area→solid-angle
/// PDF conversion, shadow test through the tracer, and a single BSDF evaluation.
/// Returns the (throughput-free) radiance contribution; the caller scales by throughput.
/// Ref: Shirley et al. 1996.
float3 evalEmissiveTriangleNEE<T : ITracer>(
    T tracer, SurfaceHit surf,
    StructuredBuffer<GpuEmissiveTriangle> emissiveTriangles, uint emissiveTriangleCount,
    inout uint seed)
{
    if (emissiveTriangleCount == 0u) {
        return 0.0.xxx;
    }

    // Uniform triangle selection: pick index in [0, N).
    uint triIdx = min(uint(randFloat(seed) * float(emissiveTriangleCount)),
                      emissiveTriangleCount - 1u);
    GpuEmissiveTriangle tri = emissiveTriangles[triIdx];

    float3 v0    = tri.v0_area.xyz;
    float  area  = tri.v0_area.w;
    float3 edge1 = tri.edge1_emitR.xyz;
    float3 edge2 = tri.edge2_emitG.xyz;
    float3 lNorm = tri.normal_emitB.xyz; // face normal (unit), outward from emitter
    float3 Le    = float3(tri.edge1_emitR.w, tri.edge2_emitG.w, tri.normal_emitB.w);

    // Uniform area sample via Shirley sqrt-folding.
    float  sqrtU = sqrt(randFloat(seed));
    float  xi    = randFloat(seed);
    float3 bary  = float3(1.0 - sqrtU, sqrtU * (1.0 - xi), sqrtU * xi);
    float3 lPos  = v0 + edge1 * bary.y + edge2 * bary.z;

    float3 toLight = lPos - surf.position;
    float  dist    = length(toLight);
    float3 wi      = toLight / max(dist, 1.0e-6);

    float cosLight = dot(-wi, lNorm);     // positive when ray hits front face of emitter
    float cosI     = dot(wi, surf.normal); // cosine at shading surface

    if (cosLight > 0.0 && cosI > 0.0) {
        // Area PDF to solid-angle PDF: pdf_ω = (1 / (N_tri × area)) × dist² / cosLight
        float areaPdf  = 1.0 / (float(emissiveTriangleCount) * area);
        float solidPdf = areaPdf * dist * dist / cosLight;

        // Shadow ray stops just before the emitter surface to avoid self-intersection.
        // Subtract dot(geoNormal,wi)*kRayEpsilon to account for the shadow origin being
        // offset by geoNormal*kRayEpsilon — without this, surfaces whose geoNormal points
        // toward the light (e.g. a floor below a ceiling light) reach the emitter at exactly
        // tMax, causing a false occlusion (the floor always renders black).
        float3 shadowOrigin = surf.position + surf.geoNormal * kRayEpsilon;
        float  shadowTMax   = max(dist - kRayEpsilon * (1.0 + dot(surf.geoNormal, wi)), 0.0);
        if (tracer.traceOccluded(shadowOrigin, wi, shadowTMax)) {
            float3 bsdfVal = evalBSDF(surf.mat, surf.wo, wi,
                surf.normal, surf.T, surf.B, surf.coatN, surf.coatT, surf.coatB);
            return bsdfVal * Le * cosI / solidPdf;
        }
    }
    return 0.0.xxx;
}

/// Next-event estimation against the environment via 2D-separable-CDF importance sampling,
/// combined with the BSDF sampling strategy through the MIS balance heuristic.
/// Returns the (throughput-free) radiance contribution; the caller scales by throughput.
/// Ref: PBR Book 4e §13.4.3; Veach §9.2.
EnvNeeSample evalEnvNEESample<T : ITracer>(
    T tracer, SurfaceHit surf,
    StructuredBuffer<float> marginalCdf, StructuredBuffer<float> conditionalCdf,
    uint envW, uint envH, bool hasEnvMap, float envLuminanceScale,
    Texture2D<float4> envMap, SamplerState envSampler,
    inout uint seed)
{
    EnvNeeSample sample;
    sample.wi = 0.0.xxx;
    sample.envPdf = 0.0;
    sample.bsdfPdf = 0.0;
    sample.misWeight = 0.0;
    sample.contribution = 0.0.xxx;
    sample.valid = false;

    if (!hasEnvMap || envW == 0u || envH == 0u) {
        return sample;
    }

    float envPdf;
    float2 xi = float2(randFloat(seed), randFloat(seed));
    float3 wi = sampleEnvImportanceDir(marginalCdf, conditionalCdf, envW, envH, xi, envPdf);
    float cosI = dot(wi, surf.normal);
    sample.wi = wi;
    sample.envPdf = envPdf;
    if (cosI > 0.0 && envPdf > 0.0) {
        float3 shadowOrigin = surf.position + surf.geoNormal * kRayEpsilon;
        if (tracer.traceOccluded(shadowOrigin, wi, kInf)) {
            float3 Le      = sampleEnvMapLatLong(envMap, envSampler, wi) * envLuminanceScale;
            float  bsdfPdf = pdfBSDF(surf.mat, surf.wo, wi,
                surf.normal, surf.T, surf.B, surf.coatN, surf.coatT, surf.coatB);
            float  misW    = envPdf / (envPdf + bsdfPdf); // balance heuristic
            float3 bsdfVal = evalBSDF(surf.mat, surf.wo, wi,
                surf.normal, surf.T, surf.B, surf.coatN, surf.coatT, surf.coatB);
            sample.bsdfPdf = bsdfPdf;
            sample.misWeight = misW;
            sample.contribution = bsdfVal * Le * cosI * misW / envPdf;
            sample.valid = true;
        }
    }
    return sample;
}

float3 evalEnvNEE<T : ITracer>(
    T tracer, SurfaceHit surf,
    StructuredBuffer<float> marginalCdf, StructuredBuffer<float> conditionalCdf,
    uint envW, uint envH, bool hasEnvMap, float envLuminanceScale,
    Texture2D<float4> envMap, SamplerState envSampler,
    inout uint seed)
{
    return evalEnvNEESample(
        tracer, surf, marginalCdf, conditionalCdf, envW, envH, hasEnvMap, envLuminanceScale,
        envMap, envSampler, seed).contribution;
}

/// Estimate the outgoing radiance contribution at one surface vertex and advance the path.
/// Stages run in a fixed, individually-replaceable order: emission guard, stochastic
/// opacity gate, emissive-triangle NEE, environment NEE, BSDF continuation sample, and
/// Russian roulette. On termination or pass-through it returns early with state updated.
void integrateSurface<T : ITracer>(
    T tracer, SurfaceHit surf,
    uint emissiveTriangleCount, StructuredBuffer<GpuEmissiveTriangle> emissiveTriangles,
    StructuredBuffer<float> marginalCdf, StructuredBuffer<float> conditionalCdf,
    uint envW, uint envH, bool hasEnvMap, float envLuminanceScale,
    Texture2D<float4> envMap, SamplerState envSampler,
    inout PathState state)
{
    // 1) Emission. A front-face emitter hit terminates the path; the emission itself is
    //    added only when it is not already accounted for by NEE (see evalEmissionGuarded).
    float3 emission = max(surf.mat.emissionColorLum.xyz, 0.0.xxx)
                    * max(surf.mat.emissionColorLum.w, 0.0);
    if (max(emission.x, max(emission.y, emission.z)) > 0.0 && dot(surf.wo, surf.geoNormal) > 0.0) {
        state.radiance += state.throughput * evalEmissionGuarded(
            emission, state.depth, state.specularBounce, surf.wo, surf.geoNormal, emissiveTriangleCount);
        state.terminated = true;
        return;
    }

    // 2) Stochastic opacity gate (OpenPBR delta pass-through). With probability (1-opacity)
    //    the ray passes straight through with no interaction; continue it forward unchanged.
    float opacity = saturate(surf.mat.opacityFlagsPad.x);
    if (randFloat(state.seed) < (1.0 - opacity)) {
        float3 fwd = -surf.wo; // original forward (WorldRayDirection), already unit-length
        state.nextOrigin    = surf.position
            + surf.geoNormal * (dot(fwd, surf.geoNormal) >= 0.0 ? kRayEpsilon : -kRayEpsilon);
        state.nextDirection = fwd;
        state.lastBsdfPdf   = 1.0e30; // delta (needed for MIS weighting)
        state.specularBounce = true;
        return;
    }

    // 3) NEE: direct emissive-triangle area sampling.
    state.radiance += state.throughput * evalEmissiveTriangleNEE(
        tracer, surf, emissiveTriangles, emissiveTriangleCount, state.seed);

    // 4) NEE: environment importance sampling with MIS.
    state.radiance += state.throughput * evalEnvNEE(
        tracer, surf, marginalCdf, conditionalCdf, envW, envH, hasEnvMap, envLuminanceScale,
        envMap, envSampler, state.seed);

    // 5) BSDF continuation sample.
    BSDFSample bsdfSample = sampleBSDF(surf.mat, surf.wo,
        surf.normal, surf.T, surf.B, surf.coatN, surf.coatT, surf.coatB, state.seed);
    if (bsdfSample.pdf <= 0.0 ||
        max(bsdfSample.weight.x, max(bsdfSample.weight.y, bsdfSample.weight.z)) <= 0.0 ||
        !all(isfinite(bsdfSample.weight))) {
        state.terminated = true;
        return;
    }

    // 6) Advance throughput and spawn the continuation ray, offset along the geometric
    //    normal toward the side the new ray travels.
    state.specularBounce = bsdfSample.isDelta;
    state.lastBsdfPdf    = bsdfSample.isDelta ? 1.0e30 : bsdfSample.pdf;
    state.throughput    *= bsdfSample.weight;
    state.insideMedium   = bsdfSample.entersMedium;
    state.mediumSigmaT   = bsdfSample.mediumSigmaT;
    state.mediumAlbedo   = bsdfSample.mediumAlbedo;
    state.mediumG        = bsdfSample.mediumG;
    state.mediumEta      = bsdfSample.mediumEta;
    state.nextOrigin     = surf.position
        + surf.geoNormal * (dot(bsdfSample.direction, surf.geoNormal) >= 0.0 ? kRayEpsilon : -kRayEpsilon);
    state.nextDirection  = normalize(bsdfSample.direction);

    // 7) Russian roulette (after a fixed minimum depth).
    if (state.depth >= kRRMinDepth) {
        float q = saturate(max(state.throughput.x, max(state.throughput.y, state.throughput.z)));
        if (randFloat(state.seed) > q) {
            state.terminated = true;
            return;
        }
        state.throughput /= max(q, 1.0e-3);
    }
}
