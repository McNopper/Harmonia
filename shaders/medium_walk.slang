// medium_walk.slang — shared volumetric (bulk subsurface / transmission) random-walk math.
//
// Single source of truth for the chromatic hero-wavelength free-flight estimator used by BOTH
// renderers' medium walks. The two walk CONTROL FLOWS are irreducibly different, so they cannot
// share one loop:
//   • Hyperion — RT-pipeline event-driven: raygen sets the free-flight TMax, a MISS is a
//     scattering event (miss.slang), a HIT is a medium-boundary crossing (closesthit.slang's
//     shadeMediumBoundary). It must stay on TraceRay because procedural spheres (intersection
//     shader) can bound a medium, and RayQuery cannot intersect them.
//   • Theia    — self-contained compute loop (runMediumWalk in gi.comp.slang) using RayQuery.
//
// So this module exposes the per-EVENT math (free-flight sampling, scatter update, boundary
// transmittance, Fresnel exit/TIR); each renderer drives its own loop and calls these. The
// estimator math can therefore never drift again — this is the one place it lives.
//
// Pure absorbers (single-scatter albedo Λ = 0) take the exact deterministic Beer-Lambert branch
// (ratio-tracking degenerate case, zero walk variance); scattering media use the chromatic
// hero-wavelength free-flight estimator (σ_t-importance channel selection — unbiased for any
// chProb, this one minimizes variance). Offsets use kMediumEpsilon (the correct medium scale,
// much smaller than kRayEpsilon: dense media have free-flight distances below kRayEpsilon).
//
// Dielectric-sidedness contract (C7): the boundary helper takes the RAW outward-winding
// geometric normal; the caller must NOT pre-flip it. geometry_thin_walled never reaches the
// walk (no bulk interior), so it is not handled here.
//
// Refs: Novák et al. 2018, "Monte Carlo Methods for Physically-Based Volume Rendering"
//        (free-flight / null-collision / ratio-tracking estimators).
//       Kutz et al. 2017, "Spectral and Decomposition Tracking" (spectral / channel importance).
//       Burley 2015, "Extending the Disney BRDF to a BSDF with Integrated Subsurface Scattering"
//        (decoupling the walk budget from the surface bounce budget).

import common;       // kMediumEpsilon, kInf
import math;         // fresnelDielectric, randFloat, randFloat2, maxComp
import bsdf_shared;  // sampleHenyeyGreenstein

/// Hero-wavelength channel-selection probabilities: each channel weighted by its extinction σ_t
/// so near-transparent channels are not over-sampled. Unbiased for any chProb; this choice
/// (σ_t ∝ chProb) minimizes variance. The single estimator used by both renderers.
float3 mediumChannelProbs(float3 sigmaT)
{
    float3 st = max(sigmaT, 1.0e-6.xxx);
    return st / (st.x + st.y + st.z);
}

/// True for a pure absorber (single-scatter albedo Λ == 0): no collision events exist, so
/// extinction is entirely absorption and the exact Beer-Lambert transmittance applies at the
/// boundary (no free-flight sampling, no binary kill/survive variance).
bool mediumIsPureAbsorber(float3 mediumAlbedo)
{
    return maxComp(mediumAlbedo) == 0.0;
}

/// Sample a free-flight distance along the hero channel (σ_t-importance selection). Returns kInf
/// for a pure absorber (the ray runs straight to the boundary). Consumes RNG draws only for a
/// scattering medium. `mediumFreeDist`/ray TMax is set to the returned value by the caller.
float mediumSampleFreeFlight(float3 sigmaT, float3 mediumAlbedo, inout RngState seed)
{
    if (mediumIsPureAbsorber(mediumAlbedo)) {
        return kInf;
    }
    float3 chProb   = mediumChannelProbs(sigmaT);
    float3 st       = max(sigmaT, 1.0e-6.xxx);
    float  chSel    = randFloat(seed);
    float  sigmaHero = chSel < chProb.x ? st.x : (chSel < chProb.x + chProb.y ? st.y : st.z);
    float  u        = randFloat(seed);
    return -log(max(1.0 - u, 1.0e-6)) / max(sigmaHero, 1.0e-6);
}

/// In-medium scattering event (ray reached the sampled free-flight distance without a boundary):
/// throughput *= σ_s·T / pdf_coll  (σ_s = albedo·σ_t, T = exp(-σ_t·t), pdf_coll = Σ_c chProb_c·σ_t_c·T_c),
/// then Russian roulette on the walk throughput (the walk does not consume the surface bounce
/// budget), then a new direction via Henyey-Greenstein. The new origin is offset along the new
/// direction by kMediumEpsilon. On RR kill, `terminated` is set and the out values are unused.
void mediumScatterEvent(inout float3 throughput, inout RngState seed, inout bool terminated,
                        float3 sigmaT, float3 mediumAlbedo, float g,
                        float3 rayOrigin, float3 rayDir, float freeDist,
                        out float3 outOrigin, out float3 outDir)
{
    float3 st     = max(sigmaT, 1.0e-6.xxx);
    float3 chProb = mediumChannelProbs(sigmaT);
    float3 T      = exp(-st * freeDist);
    float3 sigmaS = mediumAlbedo * st;
    float  pdfColl = dot(chProb, st * T);
    throughput *= (sigmaS * T) / max(pdfColl, 1.0e-8);

    float q = saturate(maxComp(throughput));
    if (randFloat(seed) > q) {
        terminated = true;
        outOrigin  = rayOrigin;
        outDir     = rayDir;
        return;
    }
    throughput /= max(q, 1.0e-3);

    float3 scatterPos = rayOrigin + rayDir * freeDist;
    outDir   = sampleHenyeyGreenstein(rayDir, g, randFloat2(seed));
    outOrigin = scatterPos + outDir * kMediumEpsilon;
}

/// Boundary transmittance for a segment that reached geometry at distance `dist`.
/// Pure absorber: exact Beer-Lambert T = exp(-σ_t·dist). Scattering medium: T / pdf_surf where
/// pdf_surf = Σ_c chProb_c·T_c (the balance-heuristic "flight past the boundary" pdf).
void mediumBoundaryTransmittance(inout float3 throughput, float3 sigmaT, bool pureAbsorption, float dist)
{
    float3 st = max(sigmaT, 1.0e-6.xxx);
    float3 Tb = exp(-st * dist);
    if (pureAbsorption) {
        throughput *= Tb;
    } else {
        float3 chProb  = mediumChannelProbs(sigmaT);
        float  pdfSurf = dot(chProb, Tb);
        throughput *= Tb / max(pdfSurf, 1.0e-8);
    }
}

/// Resolve a medium→exterior dielectric boundary crossing: Fresnel-weighted stochastic exit
/// (refract into air, eta=1) or (total) internal reflection (stay in the medium). `outwardNormal`
/// is the RAW outward-winding geometric normal (never pre-flipped — C7). Returns true on exit
/// (caller clears `insideMedium`); false on internal reflection (caller continues the walk). The
/// (1-F) transmittance is carried by the stochastic branch selection — no throughput scaling.
/// `eta` = interior IOR. Exit/reflect offsets use kMediumEpsilon.
bool mediumResolveExit(inout RngState seed, float eta, float3 rayDir, float3 outwardNormal,
                       float3 hitPos, out float3 outOrigin, out float3 outDir)
{
    // The ray travels inside toward the boundary, so this is a medium→air crossing.
    // fresnelDielectric selects the medium→air branch (reports TIR past the critical angle) only
    // for a negative incidence cosine, so pass the negated cosine; refract() returns 0 on TIR.
    float3 d    = normalize(rayDir);
    float  cosI = abs(dot(d, outwardNormal));
    float  F    = fresnelDielectric(-cosI, eta);
    float3 refracted = refract(d, -outwardNormal, eta);
    bool   tir  = dot(refracted, refracted) < 1.0e-9;

    if (tir || randFloat(seed) < F) {
        outDir    = reflect(d, outwardNormal);
        outOrigin = hitPos - outwardNormal * kMediumEpsilon;
        return false;
    }
    outDir    = normalize(refracted);
    outOrigin = hitPos + outwardNormal * kMediumEpsilon;
    return true;
}
