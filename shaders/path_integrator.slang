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
    float3   shadowPos; ///< shadow/NEE ray origin hook (== position; kept for renderer-specific
                        ///< origin schemes — note a smooth-surface origin lift was evaluated and
                        ///< rejected: its magnitude scales with triangle size, which explodes on
                        ///< large low-poly faces. Terminator handling is the Chiang factor only.)
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
    RngState seed;
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
    float3  mediumSigmaT;
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
/// Power-proportional triangle selection (flat power CDF — a light tree's leaf sampling
/// without the tree hierarchy), Shirley sqrt-folding barycentric sample, area→solid-angle
/// PDF conversion, shadow test through the tracer, and a single BSDF evaluation.
/// The selection probability is folded back into the PDF, so the estimator stays unbiased:
/// bright emitters get proportionally more samples, all get the correct weight.
/// Returns the (throughput-free) radiance contribution; the caller scales by throughput.
/// Ref: Shirley et al. 1996 (area sampling); Conty & Kulla 2018 "Importance Sampling of
///      Many Lights…" (power-weighted leaf selection).
float3 evalEmissiveTriangleNEE<T : ITracer>(
    T tracer, SurfaceHit surf,
    StructuredBuffer<GpuEmissiveTriangle> emissiveTriangles, uint emissiveTriangleCount,
    StructuredBuffer<float> emissiveCdf,
    inout RngState seed)
{
    if (emissiveTriangleCount == 0u) {
        return 0.0.xxx;
    }

    // Power-proportional triangle selection via binary search on the prefix-sum power CDF.
    // cdf[i] = (Σ_{j≤i} power_j) / totalPower, with cdf[N-1] == 1. Find the first index
    // whose cumulative probability reaches u. 20 iterations covers 2^20 ≈ 1M triangles.
    float u  = randFloat(seed);
    uint  lo = 0u;
    uint  hi = emissiveTriangleCount - 1u;
    for (uint step = 0u; step < 20u; ++step) {
        if (lo >= hi) {
            break;
        }
        uint mid = (lo + hi) / 2u;
        if (emissiveCdf[mid] < u) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    uint triIdx = min(lo, emissiveTriangleCount - 1u);

    // Selection probability of this triangle = cdf[i] - cdf[i-1] (cdf[-1] ≡ 0). Guard the
    // all-zero-power degenerate case (black emitters) with a uniform 1/N fallback.
    float triPdf = (triIdx == 0u) ? emissiveCdf[0] : (emissiveCdf[triIdx] - emissiveCdf[triIdx - 1u]);
    if (triPdf <= 0.0) {
        triPdf = 1.0 / float(emissiveTriangleCount);
    }

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
        // Power-weighted selection folds into the area PDF: pdf_area = triPdf / area
        // (triPdf = P(select this triangle); 1/area = uniform position within it).
        // Area PDF to solid-angle PDF: pdf_ω = (triPdf / area) × dist² / cosLight.
        float areaPdf  = triPdf / area;
        float solidPdf = areaPdf * dist * dist / cosLight;

        // Shadow-terminator handling (see bsdf_shared.slang header for the full
        // rationale + references): the Chiang 2019 factor f attenuates the
        // contribution — 0 at/below the geometric horizon (no ray needed there,
        // an opaque facet receives nothing from behind itself), C1 roll-off
        // above it, exactly 1 on flat shading (Ns == Ng). The visibility ray is
        // always traced when f > 0, so no light leaks through real occluders.
        float g;
        float fTerm = shadowTerminatorFactor(surf.normal, surf.geoNormal, wi, g);
        if (fTerm > 0.0) {
            float3 shadowOrigin = surf.shadowPos + surf.geoNormal * kRayEpsilon;
            float  cosNgL       = dot(surf.geoNormal, wi);
            float  shadowTMax   = max(dist - kRayEpsilon * (1.0 + cosNgL), 0.0);
            if (tracer.traceOccluded(shadowOrigin, wi, shadowTMax)) {
                float3 bsdfVal = evalBSDF(surf.mat, surf.wo, wi,
                    surf.normal, surf.T, surf.B, surf.coatN, surf.coatT, surf.coatB);
                return bsdfVal * Le * cosI * fTerm / solidPdf;
            }
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
    inout RngState seed)
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
        // Shadow-terminator handling — same scheme as emissive NEE (Chiang 2019
        // factor; rationale + references in bsdf_shared.slang).
        float g;
        float fTerm = shadowTerminatorFactor(surf.normal, surf.geoNormal, wi, g);
        if (fTerm > 0.0) {
            float3 shadowOrigin = surf.shadowPos + surf.geoNormal * kRayEpsilon;
            if (tracer.traceOccluded(shadowOrigin, wi, kInf)) {
                float3 Le      = sampleEnvMapLatLong(envMap, envSampler, wi) * envLuminanceScale;
                float  bsdfPdf = pdfBSDF(surf.mat, surf.wo, wi,
                    surf.normal, surf.T, surf.B, surf.coatN, surf.coatT, surf.coatB);
                float  misW    = envPdf / (envPdf + bsdfPdf); // balance heuristic
                float3 bsdfVal = evalBSDF(surf.mat, surf.wo, wi,
                    surf.normal, surf.T, surf.B, surf.coatN, surf.coatT, surf.coatB);
                sample.bsdfPdf = bsdfPdf;
                sample.misWeight = misW;
                sample.contribution = bsdfVal * Le * cosI * fTerm * misW / envPdf;
                sample.valid = true;
            }
        }
    }
    return sample;
}

float3 evalEnvNEE<T : ITracer>(
    T tracer, SurfaceHit surf,
    StructuredBuffer<float> marginalCdf, StructuredBuffer<float> conditionalCdf,
    uint envW, uint envH, bool hasEnvMap, float envLuminanceScale,
    Texture2D<float4> envMap, SamplerState envSampler,
    inout RngState seed)
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
    StructuredBuffer<float> emissiveCdf,
    StructuredBuffer<float> marginalCdf, StructuredBuffer<float> conditionalCdf,
    uint envW, uint envH, bool hasEnvMap, float envLuminanceScale,
    Texture2D<float4> envMap, SamplerState envSampler,
    inout PathState state)
{
    // 1) Emission. A front-face emitter hit terminates the path; the emission itself is
    //    added only when it is not already accounted for by NEE (see evalEmissionGuarded).
    float3 emission = max(surf.mat.emissionColorLum.xyz, 0.0.xxx)
                    * max(surf.mat.emissionColorLum.w, 0.0);
    // G7 (OpenPBR spec): emission escaping through the coat is attenuated by the coat's Schlick
    // transmittance envelope (generalized_schlick color0=1-coat_F0, color90=0, exp=5) and tinted
    // by coat_color, mixed in by coat_weight. Ref: open_pbr_surface.mtlx `emission_edf`.
    float emCoatWeight = saturate(surf.mat.coatColorWeight.w);
    if (emCoatWeight > 0.0 && max(emission.x, max(emission.y, emission.z)) > 0.0) {
        float emCoatEta = max(surf.mat.coatRoughAnisoIorDark.z, 1.01);
        float emCoatF0 = sqr((emCoatEta - 1.0) / (emCoatEta + 1.0));
        float emNoV = saturate(dot(surf.wo, surf.normal));
        float emTransmit = (1.0 - emCoatF0) * (1.0 - pow(1.0 - emNoV, 5.0));
        float3 emCoatAtten = lerp(1.0.xxx, emTransmit * saturate3(surf.mat.coatColorWeight.xyz), emCoatWeight);
        emission *= emCoatAtten;
    }
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

    // 3) NEE: direct emissive-triangle area sampling (power-proportional selection).
    state.radiance += state.throughput * evalEmissiveTriangleNEE(
        tracer, surf, emissiveTriangles, emissiveTriangleCount, emissiveCdf, state.seed);

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
    //    Shadow-terminator consistency (Chiang 2019, see bsdf_shared.slang): the same
    //    shadowing that attenuates NEE must attenuate light gathered through BSDF
    //    sampling — otherwise MIS combines two different integrands and the terminator
    //    brightness depends on which technique won. The stochastic opacity pass-through
    //    (step 2) is exempt by construction (it returns earlier, no surface interaction).
    float gCont;
    float fCont = shadowTerminatorFactor(surf.normal, surf.geoNormal,
                                         normalize(bsdfSample.direction), gCont);
    if (fCont <= 0.0) {
        state.terminated = true; // sampled direction is self-shadowed by the microstructure
        return;
    }
    state.specularBounce = bsdfSample.isDelta;
    state.lastBsdfPdf    = bsdfSample.isDelta ? 1.0e30 : bsdfSample.pdf;
    state.throughput    *= bsdfSample.weight * fCont;
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
