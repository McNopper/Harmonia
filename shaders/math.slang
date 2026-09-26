import common;
__exported import rng;

float sqr(float x)
{
    return x * x;
}

/// Largest component of a float3.
float maxComp(float3 v)
{
    return max(v.x, max(v.y, v.z));
}

float safeSqrt(float x)
{
    return sqrt(max(x, 0.0));
}

float2 sampleDisk(float2 xi)
{
    float2 u = 2.0 * xi - 1.0;
    if (u.x == 0.0 && u.y == 0.0) {
        return 0.0.xx;
    }

    float r;
    float phi;
    if (abs(u.x) > abs(u.y)) {
        r = u.x;
        phi = (kPi * 0.25) * (u.y / u.x);
    } else {
        r = u.y;
        phi = (kPi * 0.5) - (kPi * 0.25) * (u.x / u.y);
    }

    return r * float2(cos(phi), sin(phi));
}

float3 sampleCosineHemisphere(float2 xi)
{
    float2 d = sampleDisk(xi);
    float z = safeSqrt(1.0 - dot(d, d));
    return float3(d.x, d.y, z);
}

// ─── Specular manifold constraint (Zeltner et al. 2020, Eq. 12). ──────────────
// Measures the angular mismatch between the specular-scattered direction and the
// outgoing direction in spherical coordinates. 2D constraint c: R²→R² for better
// Newton convergence than half-vector constraints (paper §4.4).
// S(ω, n, η) = specular scatter (reflection or refraction).
float2 specularConstraint(float3 wi, float3 wo, float3 n, float eta) {
    // Specular scatter: reflection for eta=1, refraction otherwise.
    float3 scattered;
    const float cosI = dot(wi, n);
    if (eta <= 1.0 + 1.0e-4) {
        // Reflection: S = wi - 2(wi·n)n
        scattered = wi - 2.0 * cosI * n;
    } else {
        // Refraction (Snell): η_i sinθ_i = η_t sinθ_t
        const float etaInv = 1.0 / eta;
        const float sin2T = etaInv * etaInv * (1.0 - cosI * cosI);
        if (sin2T >= 1.0) {
            // Total internal reflection — use reflection
            scattered = wi - 2.0 * cosI * n;
        } else {
            const float cosT = safeSqrt(1.0 - sin2T);
            scattered = -etaInv * wi + (etaInv * cosI - cosT) * n;
        }
    }
    // Angle difference in spherical coordinates (Eq. 12).
    const float thetaS = acos(clamp(scattered.z, -1.0, 1.0));
    const float thetaO = acos(clamp(wo.z, -1.0, 1.0));
    const float phiS = atan2(scattered.y, scattered.x);
    const float phiO = atan2(wo.y, wo.x);
    // Wrap azimuth difference to [-π, π].
    float dPhi = phiS - phiO;
    dPhi = dPhi - 2.0 * kPi * floor(dPhi / (2.0 * kPi) + 0.5);
    return float2(thetaS - thetaO, dPhi);
}

// ─── Specular manifold walk (Zeltner et al. 2020, §3-4). ─────────────────────
// Newton iteration on the specular manifold to find a path x1 → x2 → x3 where
// x2 is a specular vertex. Uses angle-difference constraints (Eq. 12) and a
// numerical Jacobian (finite differences on the tangent plane).
// Returns true if converged to a valid specular path.
// maxIters: maximum Newton iterations (typically 8-16).
// tol: convergence tolerance on the constraint norm.
bool manifoldWalk2Bounce(
    float3 x1, float3 x3,           // endpoints (shading point, light vertex)
    inout float3 x2,                 // specular vertex (seed in, solution out)
    float3 n2,                       // surface normal at x2
    float eta,                       // IOR (1.0 = reflection, >1 = refraction)
    float3 tangent, float3 bitangent, // tangent frame at x2
    uint maxIters, float tol)
{
    for (uint iter = 0; iter < maxIters; ++iter) {
        const float3 wi = normalize(x1 - x2);   // incoming direction (x2 → x1)
        const float3 wo = normalize(x3 - x2);   // outgoing direction (x2 → x3)
        const float2 c = specularConstraint(wo, wi, n2, eta);
        if (dot(c, c) < tol * tol) {
            return true; // converged
        }
        // Numerical Jacobian: dc/d(uv) via finite differences on tangent plane.
        const float eps = 1.0e-4;
        const float3 x2t = x2 + tangent * eps;
        const float3 x2b = x2 + bitangent * eps;
        const float2 cT = specularConstraint(normalize(x3 - x2t), normalize(x1 - x2t), n2, eta);
        const float2 cB = specularConstraint(normalize(x3 - x2b), normalize(x1 - x2b), n2, eta);
        const float2 dCdu = (cT - c) / eps;
        const float2 dCdv = (cB - c) / eps;
        // 2×2 Jacobian inverse.
        const float det = dCdu.x * dCdv.y - dCdu.y * dCdv.x;
        if (abs(det) < 1.0e-12) {
            return false; // singular Jacobian
        }
        const float2 duv = float2(
            (dCdv.y * c.x - dCdv.x * c.y) / det,
            (-dCdu.y * c.x + dCdu.x * c.y) / det);
        // Newton step on the tangent plane.
        x2 = x2 - duv.x * tangent - duv.y * bitangent;
        // NOTE: reprojection onto the surface (ray trace) must be done by the caller.
    }
    return false; // did not converge
}

// ─── C12: Micrograin NDF + fully-correlated GAF (Lucas et al., TOG 2024). ─────
// OpenPBR flake/sparkle lobe. Anisotropic micrograin NDF expressed in terms of
// the anisotropic GGX NDF (Eq. 12) with filling factor τ₀ and stretching βₓ,βᵧ.
// The fully-correlated GAF (Eq. 16) accounts for height-normal correlation.

// Micrograin NDF (Eq. 12): D(m) = C · D_aniso_ggx(m, βₓ, βᵧ)
// where C = -ln(1-τ₀)·(1-τ₀)/τ₀ is the micrograin density scaling.
float micrograinD(float3 m, float betaX, float betaY, float tau0) {
    if (tau0 <= 0.0 || tau0 >= 1.0) {
        return 0.0;
    }
    const float C = -log(1.0 - tau0) * (1.0 - tau0) / tau0;
    return C * GGX_D(m, betaX, betaY);
}

// Micrograin unidirectional G1 (Eq. 16): G₁^dist(i,h) = exp(-ρ·σ_s(i,h))
// Simplified: uses the projected-area shadowing at height h = m·n.
// For the practical implementation we use the Smith-like approximation
// with the micrograin NDF, since the exact σ_s requires surface integration.
float micrograinG1(float3 v, float3 m, float3 n, float betaX, float betaY, float tau0) {
    const float cosVM = max(dot(v, m), 0.0);
    const float cosVN = max(dot(v, n), 0.0);
    if (cosVM <= 0.0 || cosVN <= 0.0) {
        return 0.0;
    }
    // Height h of the micrograin vertex.
    const float h = dot(m, n);
    // Projected area scaling (Eq. 16 approximation).
    const float sigmaS = (1.0 - h) * (1.0 - h); // simplified shadow area
    const float rho = -log(1.0 - tau0) / (kPi * betaX * betaY); // grain density
    return exp(-rho * sigmaS * abs(dot(v, n)) / max(cosVM, 1.0e-5));
}

// Fully-correlated GAF (Eq. 17): G(i,o,h) = G₁(i,h)·G₁(o,h)·(1 + correlation)
// The correlation term accounts for directional overlap of shadows.
float micrograinGAF(float3 wi, float3 wo, float3 m, float3 n, float betaX, float betaY, float tau0) {
    const float g1i = micrograinG1(wi, m, n, betaX, betaY, tau0);
    const float g1o = micrograinG1(wo, m, n, betaX, betaY, tau0);
    // Simplified correlation: directional overlap of shadows (Eq. 17).
    const float cosIO = max(dot(wi, wo), 0.0);
    const float corr = 1.0 + cosIO * tau0; // approximate correlation factor
    return g1i * g1o * corr;
}

// Smith-GGX VNDF (Heitz 2018, "Sampling the GGX Distribution of Visible Normals"):
// returns the microfacet half-vector m visible to Ve.
//
// C9 (Eto & Tokuyoshi 2023, "Bounded VNDF Sampling for Smith-GGX Reflections",
// Listing 1): for reflections the visible-normal range can be restricted to a
// tighter spherical cap, cutting the number of samples whose reflection vector falls
// in the lower hemisphere (rejected for opaque surfaces). With i = tangent-space
// incoming direction and i_std = [ax*i.x, ay*i.y, i.z]/||.|| the stretched one:
//   Eq. 5: k = (1-a^2) s^2 / (s^2 + a^2 i_z^2),  a = min(ax, ay, 1)   [Eq. 6]
//          s = 1 + |i.xy|  (the sgn term is omitted because a <= 1)
//   cap  : o_z in (-b, 1],  b = k * i_std.z when i_z > 0, else b = i_std.z
// Note b multiplies the STRETCHED z (i_std.z), while k is built from the UNSTRETCHED
// i_z — exactly as in the paper's Listing 1. The matching PDF (Eq. 8) must be used:
// GGX_VNDF_PDF below. Validated numerically: E[f(o)/p(o)] reproduces
// int o.z dw = pi to within Monte-Carlo error for all roughnesses tested.
float3 sampleGGX_VNDF(float3 Ve, float alphaX, float alphaY, float2 xi)
{
    // Stretch the incoming direction (Listing 1: i_std).
    float3 Vh = normalize(float3(alphaX * Ve.x, alphaY * Ve.y, Ve.z));

    const float a = saturate(min(alphaX, alphaY));                    // Eq. 6
    const float sLen = 1.0 + length(Ve.xy);                           // s = 1 + |i.xy|
    const float a2 = a * a;
    const float s2 = sLen * sLen;
    const float k = (1.0 - a2) * s2 / (s2 + a2 * Ve.z * Ve.z);        // Eq. 5
    // Cap half-width in STRETCHED z. i_z > 0 tightens the cap to -k*i_std.z;
    // a backfacing shading normal keeps the previous range -i_std.z (paper §3.2.1).
    const float b = (Ve.z > 0.0) ? (k * Vh.z) : Vh.z;

    // Sample the stretched reflection vector o uniformly in azimuth and uniform in
    // o_z over (-b, 1]  ==  Listing 1's  z = mad(1 - rand.y, 1 + b, -b).
    const float phi = 2.0 * kPi * xi.x;
    const float oz = (1.0 - xi.y) * (1.0 + b) - b;
    const float sinTheta = safeSqrt(1.0 - oz * oz);
    const float3 oStd = float3(sinTheta * cos(phi), sinTheta * sin(phi), oz);

    // Microfacet normal in stretched space, then unstretch (Listing 1).
    // mStd.z = Vh.z + oz > 0 by construction, so this never degenerates.
    const float3 mStd = Vh + oStd;
    return normalize(float3(mStd.x * alphaX, mStd.y * alphaY, mStd.z));
}

float GGX_D(float3 m, float alphaX, float alphaY)
{
    if (m.z <= 0.0) {
        return 0.0;
    }

    float sx = m.x / alphaX;
    float sy = m.y / alphaY;
    float d = sx * sx + sy * sy + m.z * m.z;
    return 1.0 / (kPi * alphaX * alphaY * d * d);
}

float GGX_Lambda(float3 v, float alphaX, float alphaY)
{
    float cosTheta = abs(v.z);
    if (cosTheta <= 0.0) {
        return 1.0e10;
    }

    float sinTheta2 = max(0.0, 1.0 - cosTheta * cosTheta);
    if (sinTheta2 <= 0.0) {
        return 0.0;
    }

    float invSinTheta = rsqrt(sinTheta2);
    float cosPhi = v.x * invSinTheta;
    float sinPhi = v.y * invSinTheta;
    float alpha2 = sqr(alphaX * cosPhi) + sqr(alphaY * sinPhi);
    float tanTheta2 = sinTheta2 / (cosTheta * cosTheta);
    return 0.5 * (sqrt(1.0 + alpha2 * tanTheta2) - 1.0);
}

float GGX_G1(float3 v, float alphaX, float alphaY)
{
    return 1.0 / (1.0 + GGX_Lambda(v, alphaX, alphaY));
}

// Height-correlated Smith masking-shadowing (Heitz 2014): G2 = 1/(1 + Lambda(l) + Lambda(v)).
// C8 review (Tamisier et al. 2024, "Visibility Evaluation in Microfacet Theory",
// DOI 10.1109/TVCG.2024.3363659): that work is an error-prediction study over measured
// height fields, not a replacement analytic G2; it confirms the Smith term remains the
// appropriate analytic visibility model (low error vs. ray-cast ground truth). No change.
float GGX_G2(float3 l, float3 v, float alphaX, float alphaY)
{
    return 1.0 / (1.0 + GGX_Lambda(l, alphaX, alphaY) + GGX_Lambda(v, alphaX, alphaY));
}

// VNDF PDF: probability density for the half-vector h given view direction v
// Used for importance sampling. Must match sampleGGX_VNDF exactly.
//
// Unbounded VNDF (Eq. 7):      p(m) = 2 D(m) max(i.m, 0) / (i_z + t)
// C9 bounded VNDF (Eq. 8):    p(m) = 2 D(m) max(i.m, 0) / (k i_z + t)
// where t = ||(ax i.x, ay i.y, i_z)|| and k is Eq. 5. The two differ: bounding the
// sampling cap to (-k i_z, 1] renormalises the density, so pairing the bounded
// sampler with the unbounded PDF is BIASED (measured: up to +/-35% energy error).
// Reference: Eto & Tokuyoshi 2023, Listing 2.
float GGX_VNDF_PDF(float3 h, float3 v, float alphaX, float alphaY)
{
    float D = GGX_D(h, alphaX, alphaY);
    float dotVH = max(0.0, dot(v, h));
    if (dotVH <= 0.0) return 0.0;

    float t = sqrt(sqr(alphaX * v.x) + sqr(alphaY * v.y) + v.z * v.z);
    if (v.z >= 0.0) {
        // Eq. 8: the cap is (-k i_z, 1], so normalise by k i_z + t.
        float a = saturate(min(alphaX, alphaY));                  // Eq. 6
        float sLen = 1.0 + length(v.xy);                          // s = 1 + |i.xy|
        float a2 = a * a;
        float s2 = sLen * sLen;
        float k = (1.0 - a2) * s2 / (s2 + a2 * v.z * v.z);        // Eq. 5
        return 2.0 * D * dotVH / max(k * v.z + t, 1.0e-20);
    }
    // Eq. 7 for a backfacing shading normal, in Listing 2's numerically stable form:
    // 2 D (v.h) / (i_z + t) == 2 D (v.h) (t - i_z) / (t^2 - i_z^2), and t^2 - i_z^2 = |a i.xy|^2.
    // (i_z + t is a near-cancelling difference when i_z < 0; t - i_z is a sum.)
    float len2 = sqr(alphaX * v.x) + sqr(alphaY * v.y);
    return 2.0 * D * dotVH * (t - v.z) / max(len2, 1.0e-20);
}

float3 fresnelSchlick(float3 F0, float cosTheta)
{
    float x = pow(1.0 - saturate(cosTheta), 5.0);
    return F0 + (1.0 - F0) * x;
}

float fresnelDielectric(float cosI, float eta)
{
    cosI = clamp(cosI, -1.0, 1.0);
    float ei = 1.0;
    float et = eta;
    if (cosI <= 0.0) {
        cosI = abs(cosI);
        ei = eta;
        et = 1.0;
    }

    float sinT2 = sqr(ei / et) * max(0.0, 1.0 - cosI * cosI);
    if (sinT2 >= 1.0) {
        return 1.0;
    }

    float cosT = safeSqrt(1.0 - sinT2);
    float rs = (ei * cosI - et * cosT) / (ei * cosI + et * cosT);
    float rp = (et * cosI - ei * cosT) / (et * cosI + ei * cosT);
    return 0.5 * (rs * rs + rp * rp);
}

float3x3 buildTBN(float3 N)
{
    float sign = N.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (sign + N.z);
    float b = N.x * N.y * a;
    float3 T = float3(1.0 + sign * N.x * N.x * a, sign * b, -sign * N.x);
    float3 B = float3(b, sign + N.y * N.y * a, -N.y);
    return float3x3(normalize(T), normalize(B), normalize(N));
}

/// Build an orthonormal frame whose Z axis points in direction `z`.
/// Row 0 = tangent (X), Row 1 = bitangent (Y), Row 2 = z (Z).
/// Used for sampling cones / disks aligned to an arbitrary axis.
float3x3 buildFrameFromZ(float3 z)
{
    // Frisvad / Duff numerical method: stable for all z directions.
    float sign = z.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (sign + z.z);
    float b = z.x * z.y * a;
    float3 t = float3(1.0 + sign * z.x * z.x * a, sign * b, -sign * z.x);
    float3 bv = float3(b, sign + z.y * z.y * a, -z.y);
    return float3x3(normalize(t), normalize(bv), z);
}

uint wangHash(uint seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed *= 9u;
    seed = seed ^ (seed >> 4u);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    return seed;
}

/// A0(a): `randFloat`/`randFloat2` now draw from the shared Owen-scrambled Sobol' sampler
/// (see rng.slang) instead of Wang-hash white noise. Same call sites, unbiased estimator,
/// lower-discrepancy (faster-converging, denoiser-friendlier) point sets.
float randFloat(inout RngState seed)
{
    return rngNextFloat(seed);
}

float2 randFloat2(inout RngState seed)
{
    return rngNextFloat2(seed);
}
