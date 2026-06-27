import common;
import math;

struct BSDFSample {
    float3 direction;
    float3 weight;
    float  pdf;
    bool   isDelta;
};

/// Minimum specular weight — F0 at normal incidence for common dielectrics (~4 %).
static const float kMinSpecularF0 = 0.04;

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

// ─── OpenPBR 1.1 thin-film iridescence (Belcour & Barla, SIGGRAPH 2017) ───
// Spec model for the OpenPBR `thin_film` lobe, identical to MaterialX `mx_fresnel_airy`.
// A thin dielectric film over the base produces wavelength-dependent interference
// (Airy summation). This F0-driven adaptation (Schlick interfaces, base IOR recovered
// from F0) is the standard engine port (Filament/Blender), giving a deterministic
// spectral-to-RGB reflectance with no LUT.
// Refs: Belcour & Barla, "A Practical Extension to Microfacet Theory for the Modeling
//       of Varying Iridescence" (ACM TOG 2017); OpenPBR Surface v1.1 thin_film lobe.

/// XYZ spectral integration via the Belcour Gaussian-fit colour-matching sensitivity,
/// returned already converted to linear sRGB (Rec.709) so the achromatic DC reflectance
/// term can be summed directly in render space.
float3 thinFilmEvalSensitivity(float opd, float3 shift)
{
    float phase = 2.0 * kPi * opd * 1.0e-9;
    float3 val = float3(5.4856e-13, 4.4201e-13, 5.2481e-13);
    float3 pos = float3(1.6810e+06, 1.7953e+06, 2.2084e+06);
    float3 var = float3(4.3278e+09, 9.3046e+09, 6.6121e+09);
    float3 xyz = val * sqrt(2.0 * kPi * var) * cos(pos * phase + shift) * exp(-var * phase * phase);
    xyz.x += 9.7470e-14 * sqrt(2.0 * kPi * 4.5282e+09) * cos(2.2399e+06 * phase + shift.x)
           * exp(-4.5282e+09 * phase * phase);
    xyz /= 1.0685e-7;
    // XYZ → linear sRGB (Rec.709, D65).
    return float3(
        dot(float3( 3.2404542, -1.5371385, -0.4985314), xyz),
        dot(float3(-0.9692660,  1.8760108,  0.0415560), xyz),
        dot(float3( 0.0556434, -0.2040259,  1.0572252), xyz));
}

/// Recover the (real) base IOR from a normal-incidence reflectance F0.
float3 fresnel0ToIor(float3 f0)
{
    float3 s = sqrt(clamp(f0, 0.0.xxx, 0.9999.xxx));
    return (1.0.xxx + s) / max(1.0.xxx - s, 1.0e-4.xxx);
}

float thinFilmSchlick(float f0, float cosT) { return f0 + (1.0 - f0) * pow(saturate(1.0 - cosT), 5.0); }
float3 thinFilmSchlick3(float3 f0, float cosT) { return f0 + (1.0.xxx - f0) * pow(saturate(1.0 - cosT), 5.0); }

/// Belcour-Barla iridescent reflectance for a thin film over a base of reflectance `baseF0`.
/// `thicknessNm` is the physical film thickness in nm; `filmIor` the film IOR.
/// Returns linear Rec.709 RGB reflectance. Reduces to Schlick(baseF0) when thickness→0.
float3 thinFilmIridescentReflectance(float3 baseF0, float cosTheta1, float thicknessNm, float filmIor)
{
    const float outsideIor = 1.0;
    float eta2 = max(filmIor, outsideIor);
    cosTheta1 = saturate(cosTheta1);

    // Refraction angle inside the film (Snell).
    float sinTheta2Sq = sqr(outsideIor / eta2) * max(0.0, 1.0 - sqr(cosTheta1));
    if (sinTheta2Sq >= 1.0) {
        return thinFilmSchlick3(baseF0, cosTheta1); // grazing TIR — fall back to base Fresnel
    }
    float cosTheta2 = sqrt(1.0 - sinTheta2Sq);

    // Interface 1: air → film (scalar dielectric Schlick + phase).
    float R0 = sqr((outsideIor - eta2) / (outsideIor + eta2));
    float R12 = thinFilmSchlick(R0, cosTheta1);
    float T121 = 1.0 - R12;
    float phi21 = kPi - ((eta2 < outsideIor) ? kPi : 0.0); // air is rarer ⇒ phi12=0 ⇒ phi21=π

    // Interface 2: film → base (per-channel Schlick from base IOR + phase).
    float3 baseIor = fresnel0ToIor(baseF0);
    float3 d23 = (float3(eta2, eta2, eta2) - baseIor) / (float3(eta2, eta2, eta2) + baseIor);
    float3 R1 = d23 * d23;
    float3 R23 = thinFilmSchlick3(R1, cosTheta2);
    float3 phi23 = float3(baseIor.x < eta2 ? kPi : 0.0,
                          baseIor.y < eta2 ? kPi : 0.0,
                          baseIor.z < eta2 ? kPi : 0.0);

    // Optical path difference and total phase.
    float opd = 2.0 * eta2 * max(thicknessNm, 0.0) * cosTheta2;
    float3 phi = float3(phi21, phi21, phi21) + phi23;

    // Airy summation (Belcour eq. 9–11).
    float3 R123 = clamp(R12 * R23, 1.0e-5.xxx, 0.9999.xxx);
    float3 r123 = sqrt(R123);
    float3 Rs = (T121 * T121) * R23 / (1.0.xxx - R123);

    float3 I = float3(R12, R12, R12) + Rs;          // m = 0 (DC term)
    float3 Cm = Rs - float3(T121, T121, T121);
    [unroll]
    for (int m = 1; m <= 2; ++m) {
        Cm *= r123;
        float3 Sm = 2.0 * thinFilmEvalSensitivity(float(m) * opd, float(m) * phi);
        I += Cm * Sm;
    }

    // I is already in linear sRGB (Rec.709): DC reflectance summed with sRGB-converted
    // spectral terms. Clamp out-of-gamut negatives.
    return max(I, 0.0.xxx);
}

/// Backward-compatible helper for paths that modulate a normal-incidence reflectance F0
/// (e.g. the legacy split-sum IBL fallback): blends F0 toward the Belcour iridescent
/// reflectance evaluated at `cosTheta`. Returns F0 unchanged when thin-film is inactive.
float3 thinFilmTint(float3 baseF0, float cosTheta, float thicknessNm, float filmIor, float weight)
{
    if (weight <= 0.0 || thicknessNm <= 0.0) {
        return baseF0;
    }
    float3 irid = thinFilmIridescentReflectance(baseF0, cosTheta, thicknessNm, max(filmIor, 1.0));
    return saturate3(lerp(baseF0, irid, saturate(weight)));
}

/// Generalized-Schlick conductor Fresnel with F82-tint, per the OpenPBR Surface spec.
///   F_Schlick(μ) = F0 + (1 - F0)(1 - μ)^5
///   F_82(μ)      = F_Schlick(μ) - [μ(1-μ)^6 / (μ̄(1-μ̄)^6)] · [F_Schlick(μ̄) - F(μ̄)]
/// with μ̄ = 1/7 (the ~82° peak-deviation angle) and F(μ̄) = F82 · F_Schlick(μ̄),
/// so the bracketed numerator reduces to F_Schlick(μ̄)·(1 - F82).
///   F0  = reflectance at normal incidence (base_color for metals)
///   F82 = tint at the 82° grazing peak  (specular_color)
float3 fresnelF82(float3 F0, float3 F82, float cosTheta)
{
    const float muBar = 1.0 / 7.0;
    const float denom = muBar * pow(1.0 - muBar, 6.0); // μ̄(1-μ̄)^6
    float mu = saturate(cosTheta);
    float3 fSchlickBar = F0 + (1.0 - F0) * pow(1.0 - muBar, 5.0); // F_Schlick(μ̄)
    float3 a = fSchlickBar * (1.0 - F82) / denom;                 // [F_Schlick(μ̄) - F(μ̄)] / denom
    float3 fSchlick = F0 + (1.0 - F0) * pow(1.0 - mu, 5.0);
    return saturate3(fSchlick - a * mu * pow(1.0 - mu, 6.0));
}

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

float3 evalSheen(float3 color, float roughness, float3 wo, float3 wi)
{
    if (wo.z <= 0.0 || wi.z <= 0.0) {
        return 0.0.xxx;
    }

    float3 h = normalize(wo + wi);
    float fd90 = 0.5 + 2.0 * roughness * sqr(saturate(dot(wi, h)));
    float viewScatter = 1.0 + (fd90 - 1.0) * pow(1.0 - wi.z, 5.0);
    float lightScatter = 1.0 + (fd90 - 1.0) * pow(1.0 - wo.z, 5.0);
    float retro = pow(1.0 - saturate(dot(wi, h)), 5.0);
    return color * kInvPi * viewScatter * lightScatter * (0.25 + 0.75 * retro);
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
float ggxDirAlbedo(float NdotV, float alpha)
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
    float Ess = ggxDirAlbedo(NdotV, alpha);
    return 1.0.xxx + F0 * (1.0 / max(Ess, 1.0e-3) - 1.0);
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
    float3 F = fresnelF82(F0, F82, VoH);
    return evalReflectionMicrofacetWithFresnel(F, F0, wo, wi, alphaX, alphaY);
}

/// Microfacet reflection with the OpenPBR thin-film lobe: blends the base F82 Fresnel with
/// the Belcour-Barla iridescent reflectance by `tfWeight`. Evaluated at the microfacet
/// half-angle VoH so iridescence tracks the actual reflection geometry.
float3 evalReflectionMicrofacetThinFilm(float3 F0, float3 F82, float3 wo, float3 wi, float alphaX, float alphaY,
                                        float tfThicknessNm, float tfIor, float tfWeight)
{
    if (wo.z <= 0.0 || wi.z <= 0.0) {
        return 0.0.xxx;
    }
    float3 h = normalize(wo + wi);
    float VoH = saturate(dot(wo, h));
    float3 F = fresnelF82(F0, F82, VoH);
    if (tfWeight > 0.0 && tfThicknessNm > 0.0) {
        float3 Firid = thinFilmIridescentReflectance(F0, VoH, tfThicknessNm, max(tfIor, 1.0));
        F = lerp(F, Firid, saturate(tfWeight));
    }
    return evalReflectionMicrofacetWithFresnel(F, F0, wo, wi, alphaX, alphaY);
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
    specularWeight = opaqueBase * (1.0 - metalness) * max(saturate(mat.specularColorWeight.w), kMinSpecularF0);
    metalWeight = baseWeight * metalness;
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

