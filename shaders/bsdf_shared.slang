import common;
import math;

struct BSDFSample {
    float3 direction;
    float3 weight;
    float  pdf;
    bool   isDelta;
    // Step 3 (bulk subsurface random walk): set when this sample refracts INTO a bulk
    // subsurface medium instead of continuing as an ordinary surface bounce. The renderer's
    // path integrator hands off to its own volumetric random-walk loop when `entersMedium`
    // is true, using the accompanying medium parameters. Harmless/unused for renderers that
    // don't implement the walk yet (defaults to false/0).
    bool   entersMedium;
    float3 mediumSigmaT; ///< G5/G6: per-channel extinction coefficient, 1/mean-free-path (RGB)
    float3 mediumAlbedo; ///< per-channel single-scatter albedo (≈ subsurface_color / scatter)
    float  mediumG;      ///< Henyey-Greenstein anisotropy (subsurface_scatter_anisotropy)
    float  mediumEta;    ///< relative IOR of the medium's dielectric boundary (specular_ior)
};

float3 saturate3(float3 v)
{
    return clamp(v, 0.0.xxx, 1.0.xxx);
}

float luminance(float3 c)
{
    return dot(c, kRec2020Luma);
}

/// Effective transmitted color including single-scatter albedo (transmission_scatter)
/// and its anisotropy (transmission_scatter_anisotropy, g).  Energy-bounded: the result
/// is never brighter than transmission_color, so scattering can only re-tint/attenuate.
/// scatter = 0 → returns transmission_color unchanged (byte-identical default).
float3 effectiveTransColor(GpuMaterial mat)
{
    float3 transColor = saturate3(mat.transmissionColorWeight.xyz);
    float3 scatter = saturate3(mat.transmissionScatter.xyz);
    float g = clamp(mat.transmissionScatter.w, -1.0, 1.0);
    // Backscatter (g<0) makes the medium milkier; forward (g>0) keeps it clearer.
    float amount = saturate(luminance(scatter)) * saturate(0.5 * (1.0 - g));
    return transColor * lerp(1.0.xxx, scatter, amount);
}

float3 toLocal(float3 v, float3 T, float3 B, float3 N)
{
    return float3(dot(v, T), dot(v, B), dot(v, N));
}

// ── Step 3: bulk subsurface — shared volumetric random-walk primitives ──────────────────
// OpenPBR 1.1.1 models subsurface as light entering through the SAME dielectric interface as
// OpenPBR 1.1.1 models subsurface as light entering through the SAME dielectric interface as
// specular/transmission, undergoing multiple scattering in a homogeneous medium, then exiting.
// These are the shared "volume primitives" (per-channel extinction mapping + phase-function
// sampling) used by the chromatic (hero-wavelength) volumetric random walk (G5/G6). The same
// primitives serve bulk subsurface AND transmission_scatter media (the spec `anisotropic_vdf`).

/// G5: per-channel extinction σ_t.rgb = 1/(subsurface_radius · subsurface_radius_scale.rgb) for
/// the bulk subsurface medium — chromatic mean free paths (skin reddening at silhouettes, etc.).
float3 sssExtinction(float radius, float3 radiusScale)
{
    float3 mfp = max(radius, 1.0e-4) * max(saturate3(radiusScale), 1.0e-4.xxx);
    return 1.0.xxx / max(mfp, 1.0e-4.xxx);
}

/// G6: per-channel absorption/scattering coefficients for the transmission volume, per the spec
/// `dielectric_volume` VDF: σ_t = -ln(transmission_color)/transmission_depth (extinction);
/// σ_s = transmission_scatter/transmission_depth (scattering). Returns σ_t in `sigmaT` and the
/// per-channel single-scatter albedo (σ_s/σ_t) in `albedo`. Caller gates on transmission_depth>0.
void transmissionVolumeCoeffs(float3 transmissionColor, float3 transmissionScatter, float depth,
                              out float3 sigmaT, out float3 albedo)
{
    float d = max(depth, 1.0e-4);
    float3 extinction = -log(max(saturate3(transmissionColor), 1.0e-4.xxx)) / d; // absorption+scatter
    float3 scattering = max(saturate3(transmissionScatter), 0.0.xxx) / d;
    // Extinction must be >= scattering (absorption non-negative); clamp defensively.
    sigmaT = max(extinction, scattering + 1.0e-4.xxx);
    albedo = saturate3(scattering / max(sigmaT, 1.0e-4.xxx));
}

/// Henyey-Greenstein phase-function importance sample. `wi` is the ray's current travel
/// direction (pointing INTO the scatter point); returns a new travel direction distributed
/// per HG(cosTheta, g) relative to `wi` continuing forward. g=0 → isotropic (uniform sphere);
/// g>0 → forward-scattering; g<0 → back-scattering. Mean cosine of the returned distribution
/// equals `g` by construction — this is what OpenPBR's subsurface_scatter_anisotropy controls.
/// Ref: Henyey & Greenstein 1941; PBR Book 4e §11.3.2 "The Henyey-Greenstein Phase Function".
float3 sampleHenyeyGreenstein(float3 wi, float g, float2 xi)
{
    float cosTheta;
    if (abs(g) < 1.0e-3) {
        cosTheta = 1.0 - 2.0 * xi.x;
    } else {
        float sqrTerm = (1.0 - g * g) / (1.0 + g - 2.0 * g * xi.x);
        cosTheta = -(1.0 + g * g - sqrTerm * sqrTerm) / (2.0 * g);
    }
    float sinTheta = safeSqrt(1.0 - cosTheta * cosTheta);
    float phi = 2.0 * kPi * xi.y;
    float3x3 basis = buildTBN(wi);
    // Z-component is negated: the (1+g²-2g·xi)-form solves for cosTheta measured against the
    // REVERSED travel direction (standard phase-function convention); negating gives the mean
    // cosine relative to the FORWARD continuation direction, matching g's intended sign
    // (g>0 → new direction biased toward continuing forward along `wi`).
    float3 localDir = float3(sinTheta * cos(phi), sinTheta * sin(phi), -cosTheta);
    return normalize(basis[0] * localDir.x + basis[1] * localDir.y + wi * localDir.z);
}

float3 toWorld(float3 v, float3 T, float3 B, float3 N)
{
    return T * v.x + B * v.y + N * v.z;
}

void orientFrame(float3 wo, float3 N, float3 T, float3 B, out float3 Ns, out float3 Ts, out float3 Bs)
{
    if (dot(wo, N) >= 0.0) {
        Ns = N;
        Ts = T;
        Bs = B;
    } else {
        Ns = -N;
        Ts = -T;
        Bs = B;
    }
}

float sqrLength(float3 v)
{
    return dot(v, v);
}

float3 iorToF0(float eta)
{
    float x = (eta - 1.0) / max(eta + 1.0, 1.0e-4);
    return (x * x).xxx;
}

void computeAlpha(float roughness, float anisotropy, out float alphaX, out float alphaY)
{
    float r = max(roughness, 0.02);
    float a = clamp(anisotropy, 0.0, 0.98);
    alphaX = r * r * sqrt(2.0 / (1.0 + sqr(1.0 - a)));
    alphaY = (1.0 - a) * alphaX;
    alphaX = max(alphaX, 0.001);
    alphaY = max(alphaY, 0.001);
}

// ── OpenPBR 1.1.1 layer algebra (spec nodegraph, open_pbr_surface.mtlx) ──────────────────
// Exact ports of the scalar coupling formulas the MaterialX nodegraph applies between layers,
// replacing earlier approximations. See files/openpbr-1.1.1-gap-audit.md (G1-G4,G7).

/// G4: coat roughens the specular base. Spec: effective_specular_roughness =
/// mix(specular_roughness, (2·coat_r⁴ + spec_r⁴)^¼, coat_weight). Feed the RESULT to computeAlpha
/// (do NOT combine in alpha-space). Ref: open_pbr_surface.mtlx `effective_specular_roughness`.
float openpbrEffectiveSpecularRoughness(float specularRoughness, float coatRoughness, float coatWeight)
{
    float combined = pow(min(1.0, 2.0 * pow(coatRoughness, 4.0) + pow(specularRoughness, 4.0)), 0.25);
    return lerp(specularRoughness, combined, saturate(coatWeight));
}

/// G3: the dielectric base F0/eta modulated by specular_weight, and — when a coat is present —
/// seen through the coat's relative IOR. Spec: eta_s = mix(specular_ior, specular_ior/coat_ior
/// [TIR-safe inverted], coat_weight); F0 = ((eta_s-1)/(eta_s+1))²; scaled_F0 = specular_weight·F0;
/// then re-derive the modulated eta from the (sign-preserving) sqrt of the clamped scaled F0.
/// Ref: open_pbr_surface.mtlx `modulated_eta_s`. Returns the effective relative IOR.
float openpbrModulatedEta(float specularIor, float specularWeight, float coatIor, float coatWeight)
{
    float ratio = specularIor / max(coatIor, 1.0e-4);
    if (ratio < 1.0) {
        ratio = coatIor / max(specularIor, 1.0e-4); // invert to avoid TIR artifact (spec Coat-TIR fix)
    }
    float etaS = lerp(specularIor, ratio, saturate(coatWeight));
    float f0sqrt = (etaS - 1.0) / (etaS + 1.0);
    float f0 = f0sqrt * f0sqrt;
    float scaledF0 = clamp(saturate(specularWeight) * f0, 0.0, 0.99999);
    float eps = sign(etaS - 1.0) * sqrt(scaledF0);
    return (1.0 + eps) / max(1.0 - eps, 1.0e-4);
}

/// G1: physical coat darkening factor applied to the whole base substrate (a per-channel MULTIPLY,
/// NOT a view-dependent attenuation). Spec: base_darkening = (1-Kcoat)/(1-Ebase·Kcoat), with
/// Kcoat = 1-(1-coat_F0)/coat_ior², modulated toward white by coat_weight·coat_darkening. `Ebase`
/// is the base-albedo estimate (metal: base_color·specular_weight; dielectric: mix(base_color,
/// subsurface_color, subsurface_weight); blended by metalness). Ref: `base_darkening`.
float3 openpbrCoatDarkening(float3 Ebase, float coatIor, float coatWeight, float coatDarkening)
{
    float coatF0sqrt = (coatIor - 1.0) / (coatIor + 1.0);
    float coatF0 = coatF0sqrt * coatF0sqrt;
    float Kcoat = 1.0 - (1.0 - coatF0) / max(coatIor * coatIor, 1.0e-4);
    float3 darkening = (1.0 - Kcoat).xxx / max(1.0.xxx - saturate3(Ebase) * Kcoat, 1.0e-4.xxx);
    return lerp(1.0.xxx, darkening, saturate(coatWeight * coatDarkening));
}

/// G2: coat_color acts as an absorption tint on everything beneath the coat. Spec:
/// coat_attenuation = mix(1, coat_color, coat_weight), multiplied into the base substrate.
float3 openpbrCoatColorAttenuation(float3 coatColor, float coatWeight)
{
    return lerp(1.0.xxx, saturate3(coatColor), saturate(coatWeight));
}

// ─── OpenPBR 1.1.1 thin-film iridescence — faithful MaterialX mx_fresnel_airy port ───
// Spec model for the OpenPBR `thin_film` lobe (Belcour & Barla 2017), ported 1:1 from
// MaterialX `mx_fresnel_airy`: a full s/p-polarized Airy summation with the spectral
// Gaussian sensitivity, supporting BOTH a Schlick (F0/F82) dielectric base AND a
// CONDUCTOR base with complex IOR (n,k). The metal lobe uses the true conductor phase
// (mx_fresnel_conductor_phase_polarized); a real-IOR approximation cannot reproduce the
// vivid anodized-metal colour shift, so (n,k) is recovered from the artist-friendly
// (reflectivity, edge-tint) parameters via Gulbrandsen 2014 (`mx_artistic_ior`).
// Refs: Belcour & Barla, ACM TOG 2017; OpenPBR Surface v1.1.1 §thin_film;
//       MaterialX libraries/pbrlib/genglsl/lib/mx_microfacet_specular.glsl.

/// Generalized-Schlick conductor Fresnel with F82-tint (OpenPBR Surface spec).
///   F0  = reflectance at normal incidence (base_color for metals)
///   F82 = tint at the 82° grazing peak  (specular_color)
/// Declared here so the thin-film Schlick base below can reuse it.
float3 mx_fresnel_F82(float3 F0, float3 F82, float cosTheta)
{
    const float muBar = 1.0 / 7.0;
    const float denom = muBar * pow(1.0 - muBar, 6.0); // μ̄(1-μ̄)^6
    float mu = saturate(cosTheta);
    float3 fSchlickBar = F0 + (1.0 - F0) * pow(1.0 - muBar, 5.0); // F_Schlick(μ̄)
    float3 a = fSchlickBar * (1.0 - F82) / denom;                 // [F_Schlick(μ̄) - F(μ̄)] / denom
    float3 fSchlick = F0 + (1.0 - F0) * pow(1.0 - mu, 5.0);
    return saturate3(fSchlick - a * mu * pow(1.0 - mu, 6.0));
}

/// "Artist Friendly Metallic Fresnel" (Gulbrandsen 2014): map a reflectivity (F0) and
/// edge tint (F82) to a complex index of refraction (n, k) for the conductor Airy base.
void mx_artistic_ior(float3 reflectivity, float3 edgeColor, out float3 ior, out float3 extinction)
{
    float3 r = clamp(reflectivity, 0.0.xxx, 0.99.xxx);
    float3 rSqrt = sqrt(r);
    float3 nMin = (1.0.xxx - r) / (1.0.xxx + r);
    float3 nMax = (1.0.xxx + rSqrt) / max(1.0.xxx - rSqrt, float3(1.0e-4, 1.0e-4, 1.0e-4));
    ior = lerp(nMax, nMin, saturate3(edgeColor));
    float3 np1 = ior + 1.0.xxx;
    float3 nm1 = ior - 1.0.xxx;
    float3 k2 = (np1 * np1 * r - nm1 * nm1) / max(1.0.xxx - r, float3(1.0e-4, 1.0e-4, 1.0e-4));
    extinction = sqrt(max(k2, 0.0.xxx));
}

/// Polarized dielectric Fresnel (Rp, Rs) for a real relative IOR.
void mx_fresnel_dielectric_polarized(float cosTheta, float ior, out float Rp, out float Rs)
{
    float c2 = saturate(cosTheta) * saturate(cosTheta);
    float s2 = 1.0 - c2;
    float t0 = max(ior * ior - s2, 0.0);
    float t1 = t0 + c2;
    float t2 = 2.0 * sqrt(t0) * cosTheta;
    Rs = (t1 - t2) / max(t1 + t2, 1.0e-6);
    float t3 = c2 * t0 + s2 * s2;
    float t4 = t2 * s2;
    Rp = Rs * (t3 - t4) / max(t3 + t4, 1.0e-6);
}

/// Polarized conductor Fresnel (Rp, Rs) for relative complex IOR (n, k).
void mx_fresnel_conductor_polarized(float cosTheta, float3 n, float3 k, out float3 Rp, out float3 Rs)
{
    float c2 = saturate(cosTheta) * saturate(cosTheta);
    float s2 = 1.0 - c2;
    float3 n2 = n * n;
    float3 k2 = k * k;
    float3 t0 = n2 - k2 - float3(s2, s2, s2);
    float3 a2b2 = sqrt(max(t0 * t0 + 4.0 * n2 * k2, 0.0.xxx));
    float3 t1 = a2b2 + float3(c2, c2, c2);
    float3 a = sqrt(max(0.5 * (a2b2 + t0), 0.0.xxx));
    float3 t2 = 2.0 * a * cosTheta;
    Rs = (t1 - t2) / max(t1 + t2, float3(1.0e-6, 1.0e-6, 1.0e-6));
    float3 t3 = c2 * a2b2 + float3(s2 * s2, s2 * s2, s2 * s2);
    float3 t4 = t2 * s2;
    Rp = Rs * (t3 - t4) / max(t3 + t4, float3(1.0e-6, 1.0e-6, 1.0e-6));
}

/// Reflection phase shift (phiP, phiS) at a film→conductor interface (Belcour 2017).
void mx_fresnel_conductor_phase_polarized(float cosTheta, float eta1, float3 eta2, float3 kappa2,
                                     out float3 phiP, out float3 phiS)
{
    float3 k2 = kappa2 / eta2;
    float s2 = 1.0 - cosTheta * cosTheta;
    float3 A = eta2 * eta2 * (1.0.xxx - k2 * k2) - (eta1 * eta1) * float3(s2, s2, s2);
    float3 twoE2k2 = 2.0 * eta2 * eta2 * k2;
    float3 B = sqrt(max(A * A + twoE2k2 * twoE2k2, 0.0.xxx));
    float3 U = sqrt(max(0.5 * (A + B), 0.0.xxx));
    float3 V = max(0.0.xxx, sqrt(max(0.5 * (B - A), 0.0.xxx)));
    float e1c = eta1 * cosTheta;
    phiS = atan2(2.0 * eta1 * V * cosTheta, U * U + V * V - float3(e1c * e1c, e1c * e1c, e1c * e1c));
    float3 e22c = eta2 * eta2 * cosTheta;
    float3 num = 2.0 * eta1 * e22c * (2.0 * k2 * U - (1.0.xxx - k2 * k2) * V);
    float3 t = eta2 * eta2 * (1.0.xxx + k2 * k2) * cosTheta;
    float3 den = t * t - (eta1 * eta1) * (U * U + V * V);
    phiP = atan2(num, den);
}

/// Belcour Gaussian-fit colour-matching sensitivity (returns XYZ; `opd` in metres).
float3 mx_eval_sensitivity(float opd, float3 shift)
{
    float phase = 2.0 * kPi * opd;
    float3 val = float3(5.4856e-13, 4.4201e-13, 5.2481e-13);
    float3 pos = float3(1.6810e+06, 1.7953e+06, 2.2084e+06);
    float3 var = float3(4.3278e+09, 9.3046e+09, 6.6121e+09);
    float3 xyz = val * sqrt(2.0 * kPi * var) * cos(pos * phase + shift) * exp(-var * phase * phase);
    xyz.x += 9.7470e-14 * sqrt(2.0 * kPi * 4.5282e+09) * cos(2.2399e+06 * phase + shift.x)
           * exp(-4.5282e+09 * phase * phase);
    return xyz / 1.0685e-7;
}

/// XYZ → CIE RGB (neutral E illuminant), matching MaterialX mx_fresnel_airy.
float3 mx_xyz_to_rgb(float3 v)
{
    return float3(
        dot(float3( 2.3706743, -0.9000405, -0.4706338), v),
        dot(float3(-0.5138850,  1.4253036,  0.0885814), v),
        dot(float3( 0.0052982, -0.0146949,  1.0093968), v));
}

/// Recover the (real) base IOR from a normal-incidence reflectance F0.
float3 mx_f0_to_ior(float3 f0)
{
    float3 s = sqrt(clamp(f0, 0.0.xxx, 0.9999.xxx));
    return (1.0.xxx + s) / max(1.0.xxx - s, 1.0e-4.xxx);
}

/// Full Airy iridescent reflectance for a thin film over a base — faithful to MaterialX
/// mx_fresnel_airy. `isConductor` selects the complex-IOR conductor base (n,k) versus the
/// Schlick (F0/F82) dielectric base. Returns linear RGB reflectance.
float3 mx_fresnel_airy(float cosTheta, bool isConductor, float3 F0, float3 F82,
                    float3 nCond, float3 kCond, float tfThicknessNm, float tfIor)
{
    float eta1 = 1.0;
    float eta2 = max(tfIor, eta1);
    float ct = saturate(cosTheta);
    float cosTtSq = 1.0 - (1.0 - ct * ct) * (eta1 / eta2) * (eta1 / eta2);
    bool tir = (cosTtSq <= 0.0);
    float cosTt = sqrt(max(cosTtSq, 0.0));

    // First interface (air → film), polarized dielectric.
    float R12p, R12s;
    mx_fresnel_dielectric_polarized(ct, eta2 / eta1, R12p, R12s);
    if (tir) { R12p = 1.0; R12s = 1.0; }
    float T121p = 1.0 - R12p;
    float T121s = 1.0 - R12s;

    // Second interface (film → base): conductor (n,k) or Schlick dielectric.
    float3 R23p, R23s, phi23p, phi23s;
    if (isConductor) {
        mx_fresnel_conductor_polarized(cosTt, nCond / eta2, kCond / eta2, R23p, R23s);
        mx_fresnel_conductor_phase_polarized(cosTt, eta2, nCond, kCond, phi23p, phi23s);
    } else {
        float3 f = 0.5 * mx_fresnel_F82(F0, F82, cosTt);
        R23p = f;
        R23s = f;
        float3 eta3 = mx_f0_to_ior(F0);
        phi23p = float3(eta3.x < eta2 ? kPi : 0.0, eta3.y < eta2 ? kPi : 0.0, eta3.z < eta2 ? kPi : 0.0);
        phi23s = phi23p;
    }

    // First-interface phase shift (p flips past Brewster; s is always π).
    float cosB = cos(atan(eta2 / eta1));
    float phi21p = (ct < cosB) ? 0.0 : kPi;
    float phi21s = kPi;

    float3 r123p = sqrt(max(float3(R12p, R12p, R12p) * R23p, 0.0.xxx));
    float3 r123s = sqrt(max(float3(R12s, R12s, R12s) * R23s, 0.0.xxx));

    float opd = 2.0 * eta2 * cosTt * max(tfThicknessNm, 0.0) * 1.0e-9; // metres

    float3 I = 0.0.xxx;
    float3 Cm, Sm;

    // Parallel polarization.
    float3 Rsp = (T121p * T121p * R23p) / max(1.0.xxx - float3(R12p, R12p, R12p) * R23p, float3(1.0e-4, 1.0e-4, 1.0e-4));
    I += float3(R12p, R12p, R12p) + Rsp;
    Cm = Rsp - float3(T121p, T121p, T121p);
    [unroll]
    for (int m = 1; m <= 2; ++m) {
        Cm *= r123p;
        Sm = 2.0 * mx_eval_sensitivity(float(m) * opd, float(m) * (phi23p + float3(phi21p, phi21p, phi21p)));
        I += Cm * Sm;
    }

    // Perpendicular polarization.
    float3 Rss = (T121s * T121s * R23s) / max(1.0.xxx - float3(R12s, R12s, R12s) * R23s, float3(1.0e-4, 1.0e-4, 1.0e-4));
    I += float3(R12s, R12s, R12s) + Rss;
    Cm = Rss - float3(T121s, T121s, T121s);
    [unroll]
    for (int m = 1; m <= 2; ++m) {
        Cm *= r123s;
        Sm = 2.0 * mx_eval_sensitivity(float(m) * opd, float(m) * (phi23s + float3(phi21s, phi21s, phi21s)));
        I += Cm * Sm;
    }

    I *= 0.5; // average the two polarizations
    return saturate3(mx_xyz_to_rgb(I));
}

/// OpenPBR thin-film Fresnel for the glossy base: the Schlick (dielectric) and conductor
/// (metal) Airy reflectances blended by metalness — matching OpenPBR's outer
/// lerp(dielectric_brdf, conductor_brdf, base_metalness).
float3 thinFilmFresnel(float metalness, float3 dielectricF0, float3 baseColor, float3 F82,
                       float cosTheta, float tfThicknessNm, float tfIor)
{
    float ior = max(tfIor, 1.0);
    float3 schlick = mx_fresnel_airy(cosTheta, false, dielectricF0, F82, 0.0.xxx, 0.0.xxx, tfThicknessNm, ior);
    if (metalness <= 0.0) {
        return schlick;
    }
    float3 n, k;
    mx_artistic_ior(baseColor, F82, n, k);
    float3 conductor = mx_fresnel_airy(cosTheta, true, baseColor, F82, n, k, tfThicknessNm, ior);
    return lerp(schlick, conductor, saturate(metalness));
}

/// Backward-compatible helper for the legacy split-sum IBL fallback (raster GI-off path):
/// modulates a normal-incidence reflectance F0 toward the (Schlick-base) Airy reflectance.
/// Returns F0 unchanged when thin-film is inactive.
float3 thinFilmTint(float3 baseF0, float cosTheta, float thicknessNm, float filmIor, float weight)
{
    if (weight <= 0.0 || thicknessNm <= 0.0) {
        return baseF0;
    }
    float3 irid = mx_fresnel_airy(cosTheta, false, baseF0, 1.0.xxx, 0.0.xxx, 0.0.xxx, thicknessNm, max(filmIor, 1.0));
    return saturate3(lerp(baseF0, irid, saturate(weight)));
}

/// Generalized-Schlick conductor Fresnel with F82-tint is defined above (moved earlier so
/// the thin-film Schlick base can reuse it).

// Fujii improved Oren-Nayar constants.
// Ref: https://mimosa-pudica.net/improved-oren-nayar.html
//      MaterialX mx_oren_nayar_compensated_diffuse
//      OpenPBR Surface spec §base/glossy-diffuse
static const float kFujiiC1 = 0.5  - 2.0 / (3.0 * kPi);        // ≈ 0.14697
static const float kFujiiC2 = 2.0 / 3.0 - 28.0 / (15.0 * kPi); // ≈ 0.06495

/// Directional albedo of the Fujii diffuse BRDF (view-angle dependent, no color).
/// Models single-scatter energy leaving a rough diffuse surface toward wo.
float diffuseDirAlbedoFujii(float cosTheta, float roughness)
{
    float A  = 1.0 / (1.0 + kFujiiC1 * roughness);
    float B  = roughness * A;
    float Si = safeSqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float G  = Si * (acos(clamp(cosTheta, -1.0, 1.0)) - Si * cosTheta)
             + 2.0 * ((Si / max(cosTheta, 1.0e-4)) * (1.0 - Si * Si * Si) - Si) / 3.0;
    return A + B * G * kInvPi;
}

/// Cosine-weighted hemisphere average of the Fujii directional albedo.
/// Used by the multi-scatter energy compensation term.
float diffuseAvgAlbedoFujii(float roughness)
{
    float A = 1.0 / (1.0 + kFujiiC1 * roughness);
    return A * (1.0 + kFujiiC2 * roughness);
}

/// OpenPBR energy-compensated diffuse BRDF (Fujii model).
/// Single-scatter lobe + multi-scatter compensation lobe for full energy conservation.
/// wo/wi are in local (tangent) space with z = cos(theta).
float3 evalDiffuse(float3 color, float roughness, float3 wo, float3 wi)
{
    if (wo.z <= 0.0 || wi.z <= 0.0) {
        return 0.0.xxx;
    }

    float NdotV = wo.z;
    float NdotL = wi.z;
    float s     = dot(wi, wo) - NdotL * NdotV;
    float stinv = (s > 0.0) ? s / max(NdotL, NdotV) : s;

    float  A          = 1.0 / (1.0 + kFujiiC1 * roughness);
    float3 lobeSingle = color * A * (1.0 + roughness * stinv);

    float  dAlbedoV = diffuseDirAlbedoFujii(NdotV, roughness);
    float  dAlbedoL = diffuseDirAlbedoFujii(NdotL, roughness);
    float  avgA     = diffuseAvgAlbedoFujii(roughness);
    float3 colorMS  = color * color * avgA
                    / max(0.0001.xxx, 1.0.xxx - color * max(0.0, 1.0 - avgA));
    float3 lobeMS   = colorMS
                    * max(1.0e-4, 1.0 - dAlbedoV)
                    * max(1.0e-4, 1.0 - dAlbedoL)
                    / max(1.0e-4, 1.0 - avgA);

    return kInvPi * (lobeSingle + lobeMS);
}

/// Directional albedo for IBL diffuse under the OpenPBR Fujii model.
/// Replaces the plain color factor in the Lambertian IBL formula, encoding
/// the roughness-dependent energy loss plus multi-scatter compensation.
///
/// IBL usage:  Lo_diffuse = kInvPi * E_irradiance * diffuseColorDirAlbedo(NoV, roughness, color)
float3 diffuseColorDirAlbedo(float cosTheta, float roughness, float3 color)
{
    float  dAlbedo = diffuseDirAlbedoFujii(cosTheta, roughness);
    float  avgA    = diffuseAvgAlbedoFujii(roughness);
    float3 colorMS = color * color * avgA
                   / max(0.0001.xxx, 1.0.xxx - color * max(0.0, 1.0 - avgA));
    return lerp(colorMS, color, dAlbedo);
}

// ── OpenPBR sheen — Zeltner et al. 2022 ──────────────────────────────────────────────────
// "Practical Multiple-Scattering Sheen Using Linearly Transformed Cosines" — the OpenPBR-
// specified fuzz model (replacing the older Disney retroreflective sheen). Faithful port of
// MaterialX `mx_microfacet_sheen.glsl`: the LTC matrix coefficients and the sheen directional
// albedo are CLOSED-FORM rational/Gaussian fits, so NO precomputed LUT is required.
// Refs: https://github.com/tizian/ltc-sheen ;
//       MaterialX libraries/pbrlib/genglsl/lib/mx_microfacet_sheen.glsl

/// Gaussian fit to the Zeltner sheen directional-albedo table. Drives both the sheen lobe
/// magnitude and the physically-correct (view-dependent) attenuation of the layers below it.
float mx_zeltner_sheen_dir_albedo(float cosTheta, float roughness)
{
    float x = saturate(cosTheta);
    float y = clamp(roughness, 0.01, 1.0);
    float s = y * (0.0206607 + 1.58491 * y) / (0.0379424 + y * (1.32227 + y));
    float m = y * (-0.193854 + y * (-1.14885 + y * (1.7932 - 0.95943 * y * y))) / (0.046391 + y);
    float o = y * (0.000654023 + (-0.0207818 + 0.119681 * y) * y) / (1.26264 + y * (-1.92021 + y));
    return saturate(exp(-0.5 * sqr((x - m) / s)) / (s * sqrt(2.0 * kPi)) + o);
}

/// Rational fits to the inverse LTC matrix coefficients
/// (M^-1 = [[aInv, 0, bInv], [0, aInv, 0], [0, 0, 1]]).
float mx_zeltner_sheen_ltc_aInv(float cosTheta, float roughness)
{
    float x = cosTheta, y = roughness;
    return (2.58126 * x + 0.813703 * y) * y / (1.0 + 0.310327 * x * x + 2.60994 * x * y);
}

float mx_zeltner_sheen_ltc_bInv(float cosTheta, float roughness)
{
    float x = cosTheta, y = roughness;
    return safeSqrt(1.0 - x) * (y - 1.0) * y * y * y
         / (0.0000254053 + 1.71228 * x - 1.71506 * x * y + 1.34174 * y * y);
}

/// Zeltner LTC sheen BRDF×cosine in the local shading frame (N = +Z). An LTC fits BRDF·cosθ
/// to a transformed clamped-cosine, so the returned value ALREADY includes the N·L cosine.
float mx_zeltner_sheen_brdf(float3 wo, float3 wi, float roughness)
{
    float NdotV = clamp(wo.z, 1.0e-4, 1.0);

    // Tangent in the plane of V and N (= +Z) to orient the LTC lobe; cross(N, X) = (-X.y, X.x, 0).
    float3 w;
    float3 Xax = float3(wo.x, wo.y, 0.0);
    float lenSq = dot(Xax, Xax);
    if (lenSq > 1.0e-8) {
        Xax *= rsqrt(lenSq);
        float3 Yax = float3(-Xax.y, Xax.x, 0.0);
        w = float3(dot(Xax, wi), dot(Yax, wi), wi.z);
    } else {
        w = wi; // V == N: any orthonormal basis works.
    }

    float aInv = mx_zeltner_sheen_ltc_aInv(NdotV, roughness);
    float bInv = mx_zeltner_sheen_ltc_bInv(NdotV, roughness);

    // wo' = M^-1 · w (back to the clamped-cosine configuration).
    float3 wo2 = float3(aInv * w.x + bInv * w.z, aInv * w.y, w.z);
    float l2 = dot(wo2, wo2);
    float Do = max(wo2.z, 0.0) * kInvPi;       // clamped-cosine PDF
    return Do * sqr(aInv / max(l2, 1.0e-8));   // Do(wo') · |M^-1| / ||M^-1·w||^4
}

/// OpenPBR fuzz/sheen lobe. Returns the BRDF WITHOUT the cosine (our eval convention — the
/// integrator multiplies by N·L), so the LTC's built-in cosine is divided back out. The lobe
/// is scaled by its directional albedo for energy conservation; the same factor attenuates the
/// layers beneath the fuzz (see `baseLayerScale` in each renderer's evalBSDF).
float3 evalSheen(float3 color, float roughness, float3 wo, float3 wi)
{
    if (wo.z <= 0.0 || wi.z <= 0.0) {
        return 0.0.xxx;
    }
    float r = clamp(roughness, 0.01, 1.0);
    float dirAlbedo = mx_zeltner_sheen_dir_albedo(wo.z, r);
    float brdfCos = mx_zeltner_sheen_brdf(wo, wi, r);
    return color * dirAlbedo * brdfCos / max(wi.z, 1.0e-4);
}

float reflectionPdf(float3 wo, float3 wi, float alphaX, float alphaY)
{
    if (wo.z <= 0.0 || wi.z <= 0.0) {
        return 0.0;
    }

    float3 h = normalize(wo + wi);
    float VoH = abs(dot(wo, h));
    if (VoH <= 0.0) {
        return 0.0;
    }

    float pdfM = GGX_D(h, alphaX, alphaY) * GGX_G1(wo, alphaX, alphaY) * VoH / max(abs(wo.z), 1.0e-5);
    return pdfM / max(4.0 * VoH, 1.0e-5);
}

/// Directional albedo of the single-scattering GGX reflection BRDF with F=1 (white-furnace
/// reflectance), via the MaterialX analytic fit (`mx_ggx_dir_albedo_analytic`). With F0=F90=1
/// the directional albedo is A+B. Used for GGX multiple-scattering energy compensation.
/// `alpha` is the GGX roughness² (isotropic; pass sqrt(alphaX*alphaY) for anisotropic).
/// Ref: MaterialX libraries/pbrlib/genglsl/lib/mx_microfacet_specular.glsl
float mx_ggx_dir_albedo(float NdotV, float alpha)
{
    float x = saturate(NdotV), y = saturate(alpha);
    float x2 = x * x, y2 = y * y;
    float4 r = float4( 0.1003,  0.9345, 1.0,    1.0   )
             + float4(-0.6303, -2.323, -1.765,   0.2281) * x
             + float4( 9.748,   2.229,  8.263,  15.94  ) * y
             + float4(-2.038,  -3.748, 11.53,  -55.83  ) * x * y
             + float4(29.34,    1.424, 28.96,   13.08  ) * x2
             + float4(-8.245,  -0.7684,-7.507,  41.26  ) * y2
             + float4(-26.44,   1.436,-36.11,   54.9   ) * x2 * y
             + float4(19.99,    0.2913,15.86,  300.2   ) * x * y2
             + float4(-5.448,   0.6286,33.37, -285.1   ) * x2 * y2;
    float2 ab = clamp(r.xy / r.zw, 0.0.xx, 1.0.xx);
    return saturate(ab.x + ab.y);
}

/// Multiple-scattering energy-compensation factor for a GGX microfacet lobe (Turquin 2019 /
/// Kulla-Conty practical form, as used by Filament). The single-scatter lobe loses energy
/// (1-Ess) to inter-microfacet shadowing; this multiplier adds it back, tinted by F0 to
/// account for Fresnel absorption on each additional scattering bounce. With F0→1 (mirror
/// metal) the factor → 1/Ess and the lobe becomes energy-preserving; with small F0
/// (dielectric specular) the correction is negligible.
/// Ref: Turquin, "Practical multiple scattering compensation for microfacet models" (2019);
///      Kulla & Conty, "Revisiting Physically Based Shading at Imageworks" (SIGGRAPH 2017).
float3 ggxMultiScatterCompensation(float3 F0, float NdotV, float alpha)
{
    float Ess = mx_ggx_dir_albedo(NdotV, alpha);
    return 1.0.xxx + F0 * (1.0 / max(Ess, 1.0e-3) - 1.0);
}

/// Multiple-scattering energy-compensation factor for the rough dielectric transmission BTDF.
/// The single-scatter lobe transmits only Ess of the energy (the rest is lost to shadowing);
/// scale by 1/Ess so rough glass stops losing energy. Dielectric (no F0 tint) → near-achromatic.
/// Ref: Turquin 2019; Kulla-Conty 2017 (dielectric variant). Caller multiplies the BTDF by this.
float ggxTransmissionMultiScatterCompensation(float NdotV, float alpha)
{
    float Ess = mx_ggx_dir_albedo(NdotV, alpha);
    return 1.0 / max(Ess, 1.0e-3);
}

/// Core microfacet reflection eval with a caller-supplied angular Fresnel `F` (already
/// evaluated at VoH). Applies GGX D·G/(4·NoL·NoV)·F plus multiple-scattering compensation
/// keyed on F0. Shared by the plain-Fresnel and thin-film wrappers below.
float3 evalReflectionMicrofacetWithFresnel(float3 F, float3 F0, float3 wo, float3 wi, float alphaX, float alphaY)
{
    if (wo.z <= 0.0 || wi.z <= 0.0) {
        return 0.0.xxx;
    }

    float3 h = normalize(wo + wi);
    float NoH = saturate(h.z);
    float NoL = saturate(wi.z);
    float NoV = saturate(wo.z);
    float VoH = saturate(dot(wo, h));
    if (NoH <= 0.0 || NoL <= 0.0 || NoV <= 0.0 || VoH <= 0.0) {
        return 0.0.xxx;
    }

    float D = GGX_D(h, alphaX, alphaY);
    float G = GGX_G2(wi, wo, alphaX, alphaY);
    float3 single = (D * G / max(4.0 * NoL * NoV, 1.0e-5)) * F;
    // GGX multiple-scattering energy compensation (recovers energy lost at high roughness).
    float alpha = sqrt(max(alphaX * alphaY, 1.0e-8));
    return single * ggxMultiScatterCompensation(F0, NoV, alpha);
}

float3 evalReflectionMicrofacet(float3 F0, float3 F82, float3 wo, float3 wi, float alphaX, float alphaY)
{
    if (wo.z <= 0.0 || wi.z <= 0.0) {
        return 0.0.xxx;
    }
    float3 h = normalize(wo + wi);
    float VoH = saturate(dot(wo, h));
    float3 F = mx_fresnel_F82(F0, F82, VoH);
    return evalReflectionMicrofacetWithFresnel(F, F0, wo, wi, alphaX, alphaY);
}

// ── OpenPBR layer stacking — directional-albedo coupling (spec §"layer") ─────────────────
// OpenPBR composites layers as R_out = R_top + (1 - E_top(ωo)) · R_base, NOT a linear blend, so
// the substrate is attenuated by the top layer's view-dependent directional albedo E_top and
// total energy never exceeds 1. These helpers return that E_top for the coat and specular layers
// (Fresnel-weighted GGX albedo), so each lower lobe can be scaled by (1 - E_above). Replaces the
// old crude `rcp(coatEta²)` coat-darkening. Ref: OpenPBR Surface v1.1.1 "Coat"/"layer";
// MaterialX libraries/pbrlib/genglsl/lib/mx_microfacet_specular.glsl directional-albedo coupling.

/// Directional albedo (reflectance) of a Fresnel-weighted GGX lobe with normal-incidence
/// reflectance F0. For a dielectric this stays ≈F0 head-on and rises to 1 at grazing (Schlick
/// directional Fresnel); it is the fraction of energy the layer removes from the substrate. NOT
/// the F=1 GGX albedo (that would over-darken a dielectric base to black at high roughness).
float fresnelGgxDirAlbedo(float NdotV, float f0, float alpha)
{
    float fc = pow(1.0 - saturate(NdotV), 5.0);
    return saturate(f0 + (1.0 - f0) * fc);
}

/// Microfacet reflection with the OpenPBR thin-film lobe. The base glossy Fresnel is
/// lerp(dielectricF0, baseColor, metalness); thin-film replaces it with the metalness-blend
/// of the Schlick (dielectric) and conductor (metal) Airy reflectances, weighted by tfWeight.
/// Evaluated at the microfacet half-angle VoH so iridescence tracks the reflection geometry.
float3 evalReflectionMicrofacetThinFilm(float3 dielectricF0, float3 baseColor, float3 F82, float metalness,
                                        float3 wo, float3 wi, float alphaX, float alphaY,
                                        float tfThicknessNm, float tfIor, float tfWeight)
{
    if (wo.z <= 0.0 || wi.z <= 0.0) {
        return 0.0.xxx;
    }
    float m = saturate(metalness);
    float3 glossyF0 = lerp(dielectricF0, baseColor, m);
    float3 h = normalize(wo + wi);
    float VoH = saturate(dot(wo, h));
    float3 F = mx_fresnel_F82(glossyF0, F82, VoH);
    if (tfWeight > 0.0 && tfThicknessNm > 0.0) {
        float3 Ftf = thinFilmFresnel(m, dielectricF0, baseColor, F82, VoH, tfThicknessNm, max(tfIor, 1.0));
        F = lerp(F, Ftf, saturate(tfWeight));
    }
    return evalReflectionMicrofacetWithFresnel(F, glossyF0, wo, wi, alphaX, alphaY);
}

// ─── A2: GGX VNDF specular dielectric microfacet BTDF (OpenPBR-correct) ───
// Replaces the Lambertian-ish evalTransmissionApprox.
// The eta² non-symmetric radiance factor lives in the PDF Jacobian, NOT here.
float3 evalTransmissionMicrofacet(float3 color, float depth, float eta, float alphaX, float alphaY, float3 wo, float3 wi)
{
    // Reject same-side rays and backfacing surfaces
    if (wo.z * wi.z >= 0.0) {
        return 0.0.xxx;
    }

    // Compute half-vector in refraction geometry (Snell's law)
    float eta_ratio = wo.z > 0.0 ? (1.0 / eta) : eta;
    float3 h = normalize(-wo * eta_ratio + wi);
    if (h.z < 0.0) h = -h;

    // Fresnel gate (1 - F) at the interface
    float F = fresnelDielectric(abs(wo.z), eta);

    // GGX VNDF microfacet BTDF: D(h) * G₂ / (4 * |cos_theta_i| * |cos_theta_t|)
    float D = GGX_D(h, alphaX, alphaY);
    float G = GGX_G2(wi, wo, alphaX, alphaY);
    float denom = 4.0 * abs(wo.z) * abs(wi.z);
    float3 f_specular = (D * G / max(denom, 1.0e-5)) * (1.0 - F);

    // Multiple-scattering compensation: a rough dielectric BTDF loses energy (1-Ess) to inter-
    // microfacet shadowing exactly like the reflection lobe (Step 2). Recover it with the same
    // Turquin/Kulla-Conty 1/Ess boost so rough glass stops darkening. Ref: Turquin 2019.
    float msAlpha = sqrt(max(alphaX * alphaY, 1.0e-8));
    f_specular *= ggxTransmissionMultiScatterCompensation(abs(wo.z), msAlpha);

    // Beer–Lambert absorption via transmission_depth + transmission_color
    float3 absorption = exp(-max(depth, 0.001) * (1.0.xxx - saturate3(color)));
    return absorption * color * f_specular;
}

// Jacobian PDF for the refracted ray, including eta² in the denominator
// (converts half-vector PDF to ray PDF via refraction Jacobian)
// Reference: Walter et al. 2007 eq. 23-24; PBR Book 4e Chapter 8.2.
float transmissionPdf(float eta, float alphaX, float alphaY, float3 wo, float3 wi, float3 h)
{
    if (wo.z * wi.z >= 0.0) {
        return 0.0;
    }

    // VNDF half-vector PDF
    float pdf_h = GGX_VNDF_PDF(h, wo, alphaX, alphaY);

    // Refraction Jacobian: pdf_ray = pdf_h / |J|
    // where |J| = eta² * (eta * |cos_theta_t| + |cos_theta_i|)²
    float cos_theta_i = abs(wo.z);
    float cos_theta_t = abs(wi.z);
    float jacobian = eta * eta * (eta * cos_theta_t + cos_theta_i) * (eta * cos_theta_t + cos_theta_i);

    return pdf_h / max(jacobian, 1.0e-5);
}

// ─── Legacy function for backward compat (only used if transmission is not sampled) ───
float3 evalTransmissionApprox(float3 color, float depth, float eta, float3 wo, float3 wi)
{
    if (wo.z * wi.z >= 0.0) {
        return 0.0.xxx;
    }

    float F = fresnelDielectric(abs(wo.z), eta);
    float3 absorption = exp(-max(depth, 0.001) * (1.0.xxx - saturate3(color)));
    return absorption * color * (1.0 - F) * kInvPi;
}

void computeLobeWeights(
    GpuMaterial mat,
    out float diffuseWeight,
    out float subsurfaceWeight,
    out float specularWeight,
    out float metalWeight,
    out float transmissionWeight,
    out float coatWeight,
    out float fuzzWeight,
    out float diffuseTransWeight,
    out bool mirrorMode,
    out bool glassMode)
{
    float opacity = saturate(mat.opacityFlagsPad.x);
    uint flags = (uint)mat.opacityFlagsPad.y;
    mirrorMode = (flags == 3u);
    glassMode = (flags == 2u);

    float baseWeight = saturate(mat.baseColorWeight.w);
    float metalness = flags == 1u ? 1.0 : saturate(mat.baseMetalnessDiffRough.x);
    float transmission = glassMode ? 1.0 : saturate(mat.transmissionColorWeight.w);
    // Transmission weight is now independent of opacity per OpenPBR: geometry_opacity gates
    // a pass-through delta BTDF (stochastic in the integrator), while transmission_weight
    // controls the specular refractive lobe independently.
    coatWeight = saturate(mat.coatColorWeight.w);
    fuzzWeight = saturate(mat.fuzzColorWeight.w);
    subsurfaceWeight = saturate(mat.subsurfaceColorWeight.w) * (1.0 - transmission);
    float opaqueBase = baseWeight * (1.0 - transmission);
    diffuseWeight = opaqueBase * (1.0 - metalness) * (1.0 - subsurfaceWeight);
    // G3 (spec): specular_weight modulates the dielectric F0/eta (see openpbrModulatedEta), NOT the
    // dielectric lobe weight — so the dielectric reflection carries the full base allocation and its
    // brightness is governed by the scaled Fresnel. For the METAL base, the spec applies
    // specular_weight directly as the generalized-Schlick BSDF weight (metal_bsdf weight=specular_weight).
    // At specular_weight=0 the spec's modulated eta → 1 (no dielectric interface → zero reflection at
    // ALL angles); our generalized-Schlick Fresnel keeps a grazing term when F0=0, so gate the
    // dielectric lobe to vanish as specular_weight→0. This is a no-op at the default specular_weight=1.
    float specularParam = saturate(mat.specularColorWeight.w);
    float dielectricPresence = smoothstep(0.0, 0.03, specularParam);
    specularWeight = opaqueBase * (1.0 - metalness) * dielectricPresence;
    metalWeight = baseWeight * metalness * specularParam;
    transmissionWeight = baseWeight * transmission;

    // Thin-walled subsurface degenerates into a diffuse reflection + diffuse transmission
    // sheet, split by the scatter anisotropy g per OpenPBR (reflect ½S(1-g), transmit ½S(1+g)).
    diffuseTransWeight = 0.0;
    bool thinWalled = mat.opacityFlagsPad.w > 0.5;
    if (thinWalled && subsurfaceWeight > 0.0) {
        float gss = clamp(mat.opacityFlagsPad.z, -1.0, 1.0);
        float ssFull = subsurfaceWeight;
        subsurfaceWeight = ssFull * 0.5 * (1.0 - gss);
        diffuseTransWeight = ssFull * 0.5 * (1.0 + gss);
    }

    float total = diffuseWeight + subsurfaceWeight + specularWeight + metalWeight + transmissionWeight + coatWeight + fuzzWeight + diffuseTransWeight;
    if (total <= 0.0) {
        diffuseWeight = 1.0;
        subsurfaceWeight = 0.0;
        specularWeight = 0.0;
        metalWeight = 0.0;
        transmissionWeight = 0.0;
        coatWeight = 0.0;
        fuzzWeight = 0.0;
        diffuseTransWeight = 0.0;
    }
}

