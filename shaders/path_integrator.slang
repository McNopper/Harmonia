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

/// Shadow-tracing abstraction: the fraction of light that reaches `origin` from
/// `origin + direction*tMax`, i.e. **1 = unoccluded, 0 = fully blocked**.
///
/// Occluders are opaque except through OpenPBR's `geometry_opacity` (α): the spec's
/// "transparent shadows" form (§Opacity/Transparency) gives the transmittance of a
/// surface with an opaque base as `T = 1 - α`, so a shadow ray accumulates ∏(1-α_i)
/// over the cut-out surfaces it crosses and stops at the first α = 1 hit. This is the
/// same `geometry_opacity × map_opacity` quantity the surface gate uses, so a cutout
/// casts a cutout shadow — the presence weight is one decision applied to every ray.
///
/// Hyperion: TraceRay against the shadow hit group, whose any-hit accumulates the
///           product (opacity micromaps let traversal skip the invocation entirely).
/// Theia:    inline RayQuery whose non-opaque-candidate loop runs the identical test.
interface ITracer {
    float traceTransmittance(float3 origin, float3 direction, float tMax);
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
    bool     backface;  ///< C7: ray struck the geometric BACK side (it is inside the
                        ///< dielectric) — the BSDF inverts its relative IOR so Fresnel/
                        ///< Snell/TIR take the exit branch. Populated from the RAW
                        ///< (outward, pre-flip) geometric normal by the renderer.
    float3   T, B;      ///< shading tangent frame
    float3   coatN;
    float3   coatT, coatB;
    GpuMaterial mat;    ///< material with texture maps pre-applied
    float3   wo;        ///< outgoing direction (toward camera/previous vertex)
};

/// Mutable path-walk state. `nextOrigin`/`nextDirection` are written by
/// integrateSurface and read back by the caller to spawn the continuation ray.
/// Gap B: "no qualifying pair yet" sentinel for PathState.captureTarget (shared by
/// the caller's gate and integrateSurface's deferred hybrid decision). captureTarget + 1
/// then wraps to 0xFFFFFFFF, which can never equal surfaceCount (>= 1 inside
/// integrateSurface), so nothing is captured and the path falls back to replay.
// Gap A (plan ae00516): maximum hybrid k the CAPTURE may select (k = captureTarget + 2)
// and the hop-array capacity that follows from it (hops = k - 2). RAISED 3 -> 4 only
// after the N-hop replay, the weight/Jacobian/footprint products and THEIA_SHIFT_MASK
// bit2 all landed (stage 2b), per the recorded ordering rule. With bit2 CLEAR a k=4
// record falls back to the UNBIASED replay - never a reconnection computed against a
// mislabelled edge - so raising the cap cannot bias anything by itself.
static const uint kMaxHybridK = 4u;
static const uint kHopCapacity = kMaxHybridK - 2u;

static const uint kCaptureNone = 0xFFFFFFFEu;

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
    /// Set when the vertex resolved to the OpenPBR opacity pass-through
    /// (`mix(ambient-medium, surface, geometry_opacity)` selected the ambient
    /// medium): the ray continued unchanged and NO surface interaction
    /// happened. Callers must NOT charge it against the bounce budget — it is a
    /// null interaction exactly like a medium free-flight segment. Charging it
    /// makes a cutout render black wherever a ray grazes several cut-out
    /// crossings, which is a budget artifact, not light transport.
    bool    passedThrough;
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
    // GI-ENH (a): reconnection capture (Theia's path reservoir only — captureRe = 1 asks
    // integrateSurface to record its FIRST full surface vertex). Pure observability: no
    // RNG draw is added, removed or reordered, so deterministic replay is unaffected and
    // the estimator output is bit-identical with capture on or off.
    uint    captureRe;
    uint    reCaptured;
    uint    surfaceCount;   ///< GI-ENH k≥3: how many surfaces integrateSurface has visited
    uint    captureTarget;  ///< GI-ENH k≥3: 0 = capture at first surface (k=2), 1 = second (k=3)
    uint    disableRR;      ///< GI-ENH §6.2.4: 1 = skip Russian roulette (replay path — avoid killing valid paths)
    uint    captureHybridMode; ///< Gap B: 1 = mask bit1 -> hybrid pair criteria pick captureTarget
    float   sampledAlphaPrev;  ///< Gap B: SAMPLED-lobe alpha at x_{k-1} (BSDFSample.lobeAlpha)
    float   arrivalDist;       ///< Gap B: ||x_k - x_{k-1}|| of the vertex being shaded
    float3  prevPosition;      ///< Gap A: origin vertex of the hop / predecessor of the edge
    float3  prevNormal;        ///< (shading normal of that origin vertex)
    ReconnectionCapture rec;
};

/// GI-ENH (a): one NEE event at a vertex, split so a reconnection shift can re-evaluate it
/// under a NEW incoming direction. `raw` carries every direction-independent factor
/// (Le, cosI, terminator, visibility, sampling-pdf division — the MIS-free form for env);
/// the shift recomputes the BSDF value and (for env) the balance-heuristic MIS weight.
struct NeeEventCapture {
    float3 wi;     ///< direction vertex -> light sample
    float3 raw;    ///< Le * cosI * fTerm * visibility / pdf (BSDF-free, MIS-free)
    float  srcPdf; ///< technique pdf (tri/sphere: solid-angle pdf incl. selection; env: envPdf)
    uint   valid;
};

/// GI-ENH (a): reconnection record of a path candidate's first FULL surface vertex — the
/// paper's compact (instance, primitive, barycentrics) reconnection vertex plus the cached
/// suffix value. Filled only when PathState.captureRe != 0.
struct ReconnectionCapture {
    float3 omegaK;         ///< continuation direction x_k -> x_{k+1}
    float3 throughputAtXk; ///< FULL prefix throughput at x_k (w_1·w_2, post-RR) — the divisor
                           ///< that normalises the cached suffix to pure incident radiance
    float3 suffixRadiance; ///< L_inc(x_k, omegaK): walk value beyond x_k, divided by throughputAtXk
    float  pdfOmegaK;      ///< p^x_k(omegaK) (1e30 for delta) — Jacobian + MIS recompute
    float  pdfEdgeIn;      ///< p^x_1(omega_1): arriving-edge BSDF pdf — Jacobian
    float  dist2X1Xk;      ///< |x_1 - x_k|^2 source side (shadow tMax at shift time)
    float  geomGSource;    ///< G(x_1 -> x_k) source side (shading-cos/dist^2 at x_k) — Jacobian
    float  geomGReverse;   ///< G(x_k -> x_1) source side (shading-cos/dist^2 at x_1) — Eq. 5
                           ///< inverse-footprint test (destination cos, both sides consistent)
    float  opacityXk;      ///< geometry_opacity at x_k (gate-eligibility at shift time)
    float3 xkPos;          ///< reconnection vertex surface identity (raw GiHit fields —
    float3 xkNormal;       ///<  rebuilt exactly into a SurfaceHit at shift time via
    float3 xkGeoNormal;    ///<  makeSurfaceHit semantics with the NEW incoming direction)
    float3 xkTangent;
    float2 xkUv;
    uint   xkMaterialIdx;
    NeeEventCapture tri;   ///< emissive-triangle/sphere NEE event at x_k (valid bit = fired)
    NeeEventCapture env;   ///< environment NEE event at x_k
    uint   flags;          ///< bit0 = captured a full vertex; bit1 = no medium entry on x_k's
                           ///< continuation; bit2 = continuation weight per-channel > 0;
                           ///< bit3 = x_k's continuation is non-delta; bit4 = primary edge
                           ///< was non-delta; bit5 = primary continuation entered no medium
    // ── GI-ENH k≥3: hop + reconnection-edge data for the hybrid extension. ──
    // For k=2 the existing fields above ARE the reconnection edge (x_1→x_k). For k=3
    // they store the HOP (x_1→x_2) and these new fields store the reconnection edge
    // (x_2→x_k). `kLevel` selects which path to try first.
    // Gap A: bounded hop array, sized kHopCapacity = kMaxHybridK - 2 (1 at the default
    // k=3, i.e. exactly the single hop the record used to store as four named fields).
    // hop[j] is the j-th replayed segment x_{j+1} -> x_{j+2} (source side).
    float3 hopDir[kHopCapacity];    ///< omega_j: replay direction of hop j
    float  hopDist2[kHopCapacity];  ///< |hop j|^2 source side
    float  hopGSource[kHopCapacity];///< G(hop j) source side - hop Jacobian
    float  hopGReverse[kHopCapacity];///< G(reverse hop j) - Eq.5 inverse footprint
    float  hopPdf[kHopCapacity];    ///< p_j(omega_{j+1}) source pdf of hop j (jacEndDenom)
    float3 omegaInK;       ///< ω_2: direction x_2 → x_k (reconnection edge for k≥3)
    float  pdfInK;         ///< p^x_2(omegaInK): reconnection-edge BSDF pdf for k=3
    float  dist2InK;       ///< |x_2 - x_k|^2 source side (k=3 reconnection edge)
    float  geomGInKSource; ///< G(x_2 → x_k) source side — k=3 Jacobian
    float  geomGInKReverse;///< G(x_k → x_2) source side — k=3 Eq. 5 inverse footprint
    uint   kLevel;         ///< 0 = no record; 1 = k=2 only; 2 = k=3 available; ≥3 = higher k
};

/// Zero-init helper — every capture record starts fully defined (uninitialized struct
/// fields carry run-to-run garbage; never let them reach a GPU buffer).
ReconnectionCapture makeEmptyReconnectionCapture() {
    ReconnectionCapture rc;
    rc.omegaK = 0.0.xxx;
    rc.throughputAtXk = 0.0.xxx;
    rc.suffixRadiance = 0.0.xxx;
    rc.pdfOmegaK = 0.0;
    rc.pdfEdgeIn = 0.0;
    rc.dist2X1Xk = 0.0;
    rc.geomGSource = 0.0;
    rc.geomGReverse = 0.0;
    rc.opacityXk = 1.0;
    rc.xkPos = 0.0.xxx;
    rc.xkNormal = 0.0.xxx;
    rc.xkGeoNormal = 0.0.xxx;
    rc.xkTangent = 0.0.xxx;
    rc.xkUv = 0.0.xx;
    rc.xkMaterialIdx = 0u;
    rc.tri.wi = 0.0.xxx;
    rc.tri.raw = 0.0.xxx;
    rc.tri.srcPdf = 0.0;
    rc.tri.valid = 0u;
    rc.env.wi = 0.0.xxx;
    rc.env.raw = 0.0.xxx;
    rc.env.srcPdf = 0.0;
    rc.env.valid = 0u;
    rc.flags = 0u;
    // GI-ENH k≥3 fields.
    for (uint j = 0u; j < kHopCapacity; ++j) {
        rc.hopDir[j] = 0.0.xxx;
        rc.hopDist2[j] = 0.0;
        rc.hopGSource[j] = 0.0;
        rc.hopGReverse[j] = 0.0;
        rc.hopPdf[j] = 0.0;
    }
    rc.omegaInK = 0.0.xxx;
    rc.pdfInK = 0.0;
    rc.dist2InK = 0.0;
    rc.geomGInKSource = 0.0;
    rc.geomGInKReverse = 0.0;
    rc.kLevel = 0u;
    return rc;
}

/// Equirectangular (lat-long) env-map lookup matching env.slang's sampleEnvMap convention.
/// Passed-in texture/sampler keep this module free of renderer globals.
float3 sampleEnvMapLatLong(Texture2D<float4> envMap, SamplerState envSampler, float3 dir)
{
    return max(envMap.SampleLevel(envSampler, dirToLatlong(dir), 0).xyz, 0.0.xxx);
}

/// The same lookup at an explicit mip level: the haze reads its in-scattered sky
/// from a strongly blurred level.
float3 sampleEnvMapLatLongLevel(Texture2D<float4> envMap, SamplerState envSampler, float3 dir, float lod)
{
    return max(envMap.SampleLevel(envSampler, dirToLatlong(dir), lod).xyz, 0.0.xxx);
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
    float3 wo, float3 geoNormal, uint emissiveTriangleCount, bool hitOnlyTexturedEmission)
{
    if (max(emission.x, max(emission.y, emission.z)) <= 0.0 || dot(wo, geoNormal) <= 0.0) {
        return 0.0.xxx;
    }
    // Textured emitters are excluded from constant-radiance NEE. Their textured EDF is carried
    // by BSDF hits at every depth; suppressing those hits would lose energy, while registering
    // their maximum as a constant light would emit from black texels and bias every receiver.
    if (hitOnlyTexturedEmission || emissiveTriangleCount == 0u || depth == 0u || specularBounce) {
        return emission;
    }
    return 0.0.xxx;
}

/// NEE against a single analytic sphere emitter via uniform solid-angle (cone) sampling.
/// From the shading point the sphere subtends a cone of half-angle θMax; a direction is
/// drawn uniformly within that cone (conditional pdf_ω = 1 / (2π·(1 - cosθMax))), the
/// closest ray/sphere intersection gives the lit point + outward normal, the shadow ray is
/// traced, and the BSDF is evaluated.
///
/// The cone pdf is only *conditional* on this sphere having been drawn from the power CDF,
/// so the full estimator denominator is `triPdf × pdf_ω` — exactly the way the triangle
/// branch folds the same selection probability into its area pdf (pdf_area = triPdf / area).
/// `triPdf` is therefore passed in by the caller; dividing by the bare cone pdf scales every
/// sphere contribution by triPdf < 1, a systematic energy loss no number of samples recovers.
///
/// Numerical note: (1 - cosθMax) is evaluated through the exact identity
/// sin²θMax / (1 + cosθMax). The direct subtraction underflows to 0 once sin²θMax falls
/// below the fp32 epsilon against 1 (radius/d ≲ 2.4e-4), making cosθMax exactly 1, the pdf
/// +inf and the contribution silently 0 for every small or distant sphere. The cone
/// direction is drawn in the same (1 - cosθ) parameterisation so it keeps full relative
/// precision there too, and the ray/sphere discriminant uses r² - |oc - (oc·wi)wi|² rather
/// than (oc·wi)² - (oc·oc - r²), whose two ≈d² terms cancel catastrophically in that regime.
/// Ref: PBR Book 4e §22.2 (sampling shapes — sphere); Wang 1992 (solid-angle cone sampling).
float3 evalEmissiveSphereNEE<T : ITracer>(
    T tracer, SurfaceHit surf, GpuEmissiveTriangle s, float triPdf, inout RngState seed,
    out NeeEventCapture cap)
{
    cap.wi = 0.0.xxx;
    cap.raw = 0.0.xxx;
    cap.srcPdf = 0.0;
    cap.valid = 0u;
    float3 center = s.v0_area.xyz;
    float  radius = s.v0_area.w;
    float3 Le     = float3(s.edge1_emitR.w, s.edge2_emitG.w, s.normal_emitB.w);

    float3 toC = center - surf.position;
    float  d   = length(toC);
    if (d <= radius) {
        return 0.0.xxx; // shading point inside the sphere — no exterior cone to sample
    }
    float3 wc     = toC / d;
    float  sinMax = min(radius / d, 1.0);
    float  cosMax = safeSqrt(1.0 - sinMax * sinMax);
    // (1 - cosθMax) via sin²/(1+cos): exact, and it never collapses to 0 the way the direct
    // subtraction does once cosθMax rounds to 1 (radius/d ≲ 2.4e-4 in fp32).
    float  oneMinusCosMax = (sinMax * sinMax) / (1.0 + cosMax);
    if (oneMinusCosMax <= 0.0) {
        return 0.0.xxx; // degenerate (zero-radius) sphere — the cone has no solid angle
    }

    // Uniform direction within the cone of half-angle θMax about axis wc, parameterised by
    // (1 - cosθ) so the tiny-cone case keeps full relative precision.
    float u1               = randFloat(seed);
    float u2               = randFloat(seed);
    float oneMinusCosTheta = u1 * oneMinusCosMax;
    float cosTheta         = 1.0 - oneMinusCosTheta;
    float sinTheta         = safeSqrt(oneMinusCosTheta * (2.0 - oneMinusCosTheta)); // √(1-cos²θ)
    float phi              = 2.0 * kPi * u2;
    float3x3 frame = buildFrameFromZ(wc); // rows: T, B, wc (Z axis)
    float3 wi      = normalize(frame[0] * (sinTheta * cos(phi))
                             + frame[1] * (sinTheta * sin(phi))
                             + wc * cosTheta);

    // Closest ray/sphere intersection (wi is unit → a == 1). oc = origin - center.
    // disc is the stable form r² - |oc⊥|²: the textbook (oc·wi)² - (oc·oc - r²) subtracts
    // two ≈d² quantities and loses every bit of the answer for a small distant sphere.
    float3 oc   = surf.position - center;
    float  hb   = dot(oc, wi);               // half-b
    float3 ocT  = oc - hb * wi;              // component of oc perpendicular to wi
    float  disc = radius * radius - dot(ocT, ocT);
    if (disc <= 0.0) {
        return 0.0.xxx; // numerical guard: sampled direction grazed past the sphere
    }
    float t = -hb - sqrt(disc);              // nearest (front) intersection
    if (t <= 0.0) {
        return 0.0.xxx;
    }
    float3 nLight = normalize((surf.position + wi * t) - center); // outward sphere normal
    float  cosL   = dot(-wi, nLight);        // > 0: ray meets the outward face
    float  cosI   = dot(wi, surf.normal);    // cosine at the shading surface

    if (cosL <= 0.0 || cosI <= 0.0) {
        return 0.0.xxx;
    }

    // Estimator denominator: P(select this emitter from the power CDF) × the uniform-cone
    // solid-angle pdf it is conditioned on. Mirrors the triangle branch's triPdf / area.
    float conePdf  = 1.0 / (2.0 * kPi * oneMinusCosMax);
    float solidPdf = triPdf * conePdf;

    // Shadow-terminator handling — same Chiang 2019 factor as the triangle NEE branch.
    float g;
    float fTerm = shadowTerminatorFactor(surf.normal, surf.geoNormal, wi, g);
    if (fTerm > 0.0) {
        float3 shadowOrigin = surf.shadowPos + surf.geoNormal * kRayEpsilon;
        float  cosNgL       = dot(surf.geoNormal, wi);
        float  shadowTMax   = max(t - kRayEpsilon * (1.0 + cosNgL), 0.0);
        float visibility = tracer.traceTransmittance(shadowOrigin, wi, shadowTMax);
        if (visibility > 0.0) {
            float3 bsdfVal = evalBSDF(surf.mat, surf.wo, wi,
                surf.normal, surf.T, surf.B, surf.coatN, surf.coatT, surf.coatB, surf.backface);
            cap.wi = wi;
            cap.raw = Le * cosI * fTerm * visibility / solidPdf;
            cap.srcPdf = solidPdf;
            cap.valid = 1u;
            return bsdfVal * Le * cosI * fTerm * visibility / solidPdf;
        }
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
    inout RngState seed, out NeeEventCapture cap)
{
    cap.wi = 0.0.xxx;
    cap.raw = 0.0.xxx;
    cap.srcPdf = 0.0;
    cap.valid = 0u;
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

    // Tagged-union dispatch: analytic sphere emitters are cone-sampled in solid angle;
    // triangle emitters take the area-sampling path below. Both share the single power CDF,
    // so both must divide by triPdf — the sphere branch folds it into its cone pdf.
    if (tri.kind_pad.x == kEmissiveSphere) {
        return evalEmissiveSphereNEE(tracer, surf, tri, triPdf, seed, cap);
    }

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
            float visibility = tracer.traceTransmittance(shadowOrigin, wi, shadowTMax);
            if (visibility > 0.0) {
                float3 bsdfVal = evalBSDF(surf.mat, surf.wo, wi,
                    surf.normal, surf.T, surf.B, surf.coatN, surf.coatT, surf.coatB, surf.backface);
                cap.wi = wi;
                cap.raw = Le * cosI * fTerm * visibility / solidPdf;
                cap.srcPdf = solidPdf;
                cap.valid = 1u;
                return bsdfVal * Le * cosI * fTerm * visibility / solidPdf;
            }
        }
    }
    return 0.0.xxx;
}

/// Next-event estimation against the environment via 2D-separable-CDF importance sampling,
/// combined with the BSDF sampling strategy through the MIS balance heuristic.
/// Returns the (throughput-free) radiance contribution; the caller scales by throughput.
/// Ref: PBR Book 4e §13.4.3; Veach §9.2.
/// Direct illumination from the scene's analytic lights. Point, spot and directional lights
/// are evaluated once; a rectangular light is sampled uniformly over its area. These lights
/// have no geometry for BSDF rays to hit, so no MIS term is needed.
float3 evalAnalyticLightNEE<T : ITracer>(
    T tracer, SurfaceHit surf, StructuredBuffer<GpuLight> lights, uint lightCount, inout RngState seed)
{
    float3 result = 0.0.xxx;
    for (uint i = 0u; i < lightCount; ++i) {
        GpuLight light = lights[i];
        const uint lt = asuint(light.typeF);
        float3 wi = 0.0.xxx;
        float3 Li = 0.0.xxx;
        float tMax = 1.0e16;
        float NdotL = 0.0;
        float cosL = 1.0;
        float areaWeight = 1.0;
        if (lt == kLightRect) {
            const float3 Ln = normalize(light.direction);
            const float3 up = abs(Ln.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
            const float3 Lt0 = normalize(cross(up, Ln));
            const float3 Lb0 = cross(Ln, Lt0);
            const float roll = light.cosInner;
            const float3 Lt = cos(roll) * Lt0 + sin(roll) * Lb0;
            const float3 Lb = cross(Ln, Lt);
            const float2 xi = randFloat2(seed) * 2.0 - 1.0;
            const float3 sp = light.position + Lt * (xi.x * light.halfWidth) + Lb * (xi.y * light.halfHeight);
            const float3 toLight = sp - surf.position;
            const float distSq = max(dot(toLight, toLight), 1.0e-12);
            const float dist = sqrt(distSq);
            wi = toLight / dist;
            NdotL = max(dot(surf.normal, wi), 0.0);
            cosL = max(dot(-wi, Ln), 0.0);
            if (NdotL <= 0.0 || cosL <= 0.0) {
                continue;
            }
            Li = light.color * light.intensity;
            areaWeight = 4.0 * light.halfWidth * light.halfHeight / distSq;
            tMax = dist;
        } else if (lt == kLightPoint || lt == kLightSpot) {
            const float3 toLight = light.position - surf.position;
            const float distSq = max(dot(toLight, toLight), 1.0e-12);
            const float dist = sqrt(distSq);
            if (light.range > 0.0 && dist > light.range) {
                continue;
            }
            wi = toLight / dist;
            float attenuation = 1.0;
            if (lt == kLightSpot) {
                const float cosA = dot(-wi, normalize(light.direction));
                const float spot = saturate((cosA - light.cosOuter) / max(light.cosInner - light.cosOuter, 1.0e-4));
                if (spot <= 0.0) {
                    continue;
                }
                attenuation = spot;
            }
            NdotL = max(dot(surf.normal, wi), 0.0);
            if (NdotL <= 0.0) {
                continue;
            }
            Li = light.color * (light.intensity * attenuation / distSq);
            tMax = dist;
        } else if (lt == kLightDirectional) {
            wi = -normalize(light.direction);
            NdotL = max(dot(surf.normal, wi), 0.0);
            if (NdotL <= 0.0) {
                continue;
            }
            Li = light.color * light.intensity;
        } else {
            continue;
        }
        float g;
        const float fTerm = shadowTerminatorFactor(surf.normal, surf.geoNormal, wi, g);
        if (fTerm <= 0.0) {
            continue;
        }
        const float3 shadowOrigin = surf.shadowPos + surf.geoNormal * kRayEpsilon;
        const float cosNgL = dot(surf.geoNormal, wi);
        const float shadowTMax = max(tMax - kRayEpsilon * (1.0 + cosNgL), 0.0);
        const float visibility = tracer.traceTransmittance(shadowOrigin, wi, shadowTMax);
        if (visibility <= 0.0) {
            continue;
        }
        const float3 bsdfValue = evalBSDF(surf.mat, surf.wo, wi, surf.normal, surf.T, surf.B,
                                          surf.coatN, surf.coatT, surf.coatB, surf.backface);
        result += bsdfValue * Li * NdotL * cosL * areaWeight * visibility * fTerm;
    }
    return result;
}

EnvNeeSample evalEnvNEESample<T : ITracer>(
    T tracer, SurfaceHit surf,
    StructuredBuffer<float> marginalCdf, StructuredBuffer<float> conditionalCdf,
    uint envW, uint envH, bool hasEnvMap, float envLuminanceScale,
    Texture2D<float4> envMap, SamplerState envSampler,
    inout RngState seed, out NeeEventCapture cap)
{
    cap.wi = 0.0.xxx;
    cap.raw = 0.0.xxx;
    cap.srcPdf = 0.0;
    cap.valid = 0u;
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
            float visibility = tracer.traceTransmittance(shadowOrigin, wi, kInf);
            if (visibility > 0.0) {
                float3 Le      = sampleEnvMapLatLong(envMap, envSampler, wi) * envLuminanceScale;
                float  bsdfPdf = pdfBSDF(surf.mat, surf.wo, wi,
                    surf.normal, surf.T, surf.B, surf.coatN, surf.coatT, surf.coatB, surf.backface);
                float  misW    = envPdf / (envPdf + bsdfPdf); // balance heuristic
                float3 bsdfVal = evalBSDF(surf.mat, surf.wo, wi,
                    surf.normal, surf.T, surf.B, surf.coatN, surf.coatT, surf.coatB, surf.backface);
                sample.bsdfPdf = bsdfPdf;
                sample.misWeight = misW;
                sample.contribution = bsdfVal * Le * cosI * fTerm * misW * visibility / envPdf;
                sample.valid = true;
                // GI-ENH (a) capture: MIS-free raw form (the shift recomputes misW under the
                // new incoming direction).
                cap.wi = wi;
                cap.raw = Le * cosI * fTerm * visibility / envPdf;
                cap.srcPdf = envPdf;
                cap.valid = 1u;
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
    inout RngState seed, out NeeEventCapture cap)
{
    return evalEnvNEESample(
        tracer, surf, marginalCdf, conditionalCdf, envW, envH, hasEnvMap, envLuminanceScale,
        envMap, envSampler, seed, cap).contribution;
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
    StructuredBuffer<GpuLight> lights, uint lightCount, bool analyticLightNee,
    inout PathState state)
{
    state.passedThrough = false;

    // 1) Stochastic opacity gate — the OpenPBR `mix(S_ambient-medium, M_surface, α)` with
    //    α = geometry_opacity × map_opacity (spec §"Opacity / Transparency"). With
    //    probability (1-α) the ambient medium is selected: no surface exists here, so the
    //    ray continues straight through with no interaction: neither its BSDF nor its EDF is
    //    evaluated. `passedThrough` tells the caller this consumed no bounce (a null interaction).
    float opacity = saturate(surf.mat.opacityFlagsPad.x);
    if (randFloat(state.seed) < (1.0 - opacity)) {
        float3 fwd = -surf.wo; // original forward (WorldRayDirection), already unit-length
        state.nextOrigin    = surf.position
            + surf.geoNormal * (dot(fwd, surf.geoNormal) >= 0.0 ? kNullInteractionEpsilon
                                                                : -kNullInteractionEpsilon);
        state.nextDirection = fwd;
        state.passedThrough = true;
        return;
    }

    // 2) Emission. OpenPBR's surface constructor carries BOTH fuzz_layer (BSDF) and
    //    emission_edf (EDF): emission adds to the surface response; it never replaces the
    //    BSDF or terminates the path. evalEmissionGuarded only prevents double-counting the
    //    EDF against NEE. The surface still receives NEE and advances through its BSDF below.
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
            emission, state.depth, state.specularBounce, surf.wo, surf.geoNormal, emissiveTriangleCount,
            surf.mat.baseMetalnessDiffRough.z > 0.5);
    }

    // 3) NEE: direct emissive-triangle area sampling (power-proportional selection).
    // GI-ENH (a): capture the NEE events of the target FULL vertex when requested — pure
    // observability, no RNG added or reordered. GI-ENH k≥3: captureTarget selects which
    // surface to capture (0 = first, 1 = second) for the hybrid reconnection shift.
    ++state.surfaceCount;
    bool captureHere = (state.captureRe != 0u) && (state.reCaptured == 0u) &&
                             (state.surfaceCount == state.captureTarget + 1u);
    NeeEventCapture capTri;
    state.radiance += state.throughput * evalEmissiveTriangleNEE(
        tracer, surf, emissiveTriangles, emissiveTriangleCount, emissiveCdf, state.seed, capTri);

    // 3b) NEE: analytic point, spot, directional and rectangular lights.
    if (analyticLightNee) {
        state.radiance += state.throughput * evalAnalyticLightNEE(
            tracer, surf, lights, lightCount, state.seed);
    }

    // 4) NEE: environment importance sampling with MIS.
    NeeEventCapture capEnv;
    state.radiance += state.throughput * evalEnvNEE(
        tracer, surf, marginalCdf, conditionalCdf, envW, envH, hasEnvMap, envLuminanceScale,
        envMap, envSampler, state.seed, capEnv);

    // 5) BSDF continuation sample.
    BSDFSample bsdfSample = sampleBSDF(surf.mat, surf.wo,
        surf.normal, surf.T, surf.B, surf.coatN, surf.coatT, surf.coatB, surf.backface, state.seed);
    if (bsdfSample.pdf <= 0.0 ||
        max(bsdfSample.weight.x, max(bsdfSample.weight.y, bsdfSample.weight.z)) <= 0.0 ||
        !all(isfinite(bsdfSample.weight))) {
        state.terminated = true;
        return;
    }

    // Gap B: the HYBRID pair decision is made HERE, immediately after this vertex's
    // lobe is sampled, because the paper's criterion is PAIRWISE over the SAMPLED
    // lobes - min(alpha_{x_{k-1}}, alpha_{x_k}) >= alpha_min (ReSTIR PT Enhanced 2.3
    // "with alpha from the path's sampled lobes"; Foundations 7.6 "we examine the
    // roughness of just the selected lobe"). At the caller's arrival point alpha_{x_k}
    // does not exist yet - the sampleBSDF above produces it - and captureHere must be
    // settled before the NEE capture below, so the test cannot run at arrival
    // (site map: Theia/PLAN.md b4fd92a). Reconnection mode never enters here.
    if (state.captureRe != 0u && state.captureHybridMode != 0u &&
        state.reCaptured == 0u && state.captureTarget == kCaptureNone &&
        state.surfaceCount < kMaxHybridK) {
        const float kAlphaMin = 0.2F; // DQLin/ReSTIR_PT Params.slang specularRoughnessThreshold
        const float kDMin = 0.1F;     // DQLin/ReSTIR_PT Params.slang nearFieldDistance
        const bool pairRough = min(state.sampledAlphaPrev, bsdfSample.lobeAlpha) >= kAlphaMin;
        if (pairRough && state.arrivalDist >= kDMin) {
            // surfaceCount was already incremented above, so the capture index is
            // surfaceCount - 1 (identical to the caller's pre-increment assignment).
            state.captureTarget = state.surfaceCount - 1u;
            if (state.captureTarget != 0u) {
                // Gap A stage 2: the reconnection EDGE is captured where captureTarget
                // is decided, so it is always x_{k-1} -> x_k for ANY k (the old code
                // hard-wired the bounce-0 vertex, correct only for k=3). These two still
                // hold x_{k-1}'s sample here - this runs before they are overwritten below.
                state.rec.omegaInK = normalize(state.nextDirection);
                state.rec.pdfInK = state.lastBsdfPdf;
            }
            captureHere = (state.surfaceCount == state.captureTarget + 1u);
        }
    }
    if (captureHere) {
        state.rec.tri = capTri;
        state.rec.env = capEnv;
        state.rec.opacityXk = saturate(surf.mat.opacityFlagsPad.x);
    }
    // Gap B: remember THIS vertex's sampled-lobe alpha for the next pair test.
    state.sampledAlphaPrev = bsdfSample.lobeAlpha;

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
    if (captureHere) {
        // GI-ENH (a): the reconnection record's suffix anchor — x_k's continuation
        // direction/pdf (the walk's own factors, not a re-derivation). The throughput
        // divisor is captured at the harvest site (post-RR, full prefix w_1·w_2).
        state.rec.omegaK = normalize(bsdfSample.direction);
        state.rec.pdfOmegaK = bsdfSample.isDelta ? 1.0e30 : bsdfSample.pdf;
        state.rec.flags |= 1u
            | (bsdfSample.entersMedium ? 0u : 2u)
            | (bsdfSample.isDelta ? 0u : 8u);
        state.reCaptured = 1u;
    }
    state.insideMedium   = bsdfSample.entersMedium;
    state.mediumSigmaT   = bsdfSample.mediumSigmaT;
    state.mediumAlbedo   = bsdfSample.mediumAlbedo;
    state.mediumG        = bsdfSample.mediumG;
    state.mediumEta      = bsdfSample.mediumEta;
    state.nextOrigin     = surf.position
        + surf.geoNormal * (dot(bsdfSample.direction, surf.geoNormal) >= 0.0 ? kRayEpsilon : -kRayEpsilon);
    state.nextDirection  = normalize(bsdfSample.direction);

    // 7) Russian roulette (after a fixed minimum depth).
    // GI-ENH §6.2.4: disabled during replay — roulette failures at shift mapping kill
    // valid paths that spatiotemporal reuse would otherwise exploit.
    if (state.depth >= kRRMinDepth && state.disableRR == 0u) {
        float q = saturate(max(state.throughput.x, max(state.throughput.y, state.throughput.z)));
        if (randFloat(state.seed) > q) {
            state.terminated = true;
            return;
        }
        state.throughput /= max(q, 1.0e-3);
    }
}
