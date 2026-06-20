// env_sample.slang — pure 2D separable CDF env-map importance sampling.
//
// No global state, no imports. Both Hyperion (via env.slang wrappers that pass
// their global CDF buffers) and Theia (ibl_diffuse.comp.slang, directly) use
// these functions.
//
// Ref: Pharr, Jakob & Humphreys — "Physically Based Rendering" 4th ed. §12.5
//      Veach — "Robust Monte Carlo Methods" §9.2

static const float kEnvSamplePi    = 3.14159265359;
static const float kEnvSampleInvPi = 0.31830988618;
static const float kEnvSampleEps   = 1.0e-6;

float sanitizePdf(float pdf)
{
    return (isfinite(pdf) && pdf > 0.0) ? pdf : 0.0;
}

float balanceHeuristic(float pA, float pB)
{
    pA = max(sanitizePdf(pA), 0.0);
    pB = max(sanitizePdf(pB), 0.0);
    const float denom = pA + pB;
    return (denom > kEnvSampleEps) ? (pA / denom) : 0.0;
}

/// Binary search: largest i ∈ [0, n-1] such that cdf[base+i+1] <= r.
/// CDF has (n+1) entries at [base .. base+n] with cdf[base]=0, cdf[base+n]=1.
uint sampleCdf1D(StructuredBuffer<float> cdf, uint base, uint n, float r)
{
    uint lo = 0u, hi = n - 1u;
    while (lo < hi) {
        uint mid = (lo + hi) >> 1u;
        if (cdf[base + mid + 1u] <= r) lo = mid + 1u;
        else                            hi = mid;
    }
    return lo;
}

/// Importance-sample the env map from its pre-built 2D separable CDF.
/// xi: two uniform random numbers in [0,1)² (PRNG pair or Hammersley).
/// Returns the sampled world-space direction; pdfOmega is the solid-angle PDF.
float3 sampleEnvImportanceDir(
    StructuredBuffer<float> marginalCdf,
    StructuredBuffer<float> conditionalCdf,
    uint W, uint H, float2 xi, out float pdfOmega)
{
    if (W == 0u || H == 0u) {
        pdfOmega = 0.0;
        return float3(0.0, 1.0, 0.0);
    }

    // Sample row via marginal CDF, column via conditional CDF for that row.
    uint row = sampleCdf1D(marginalCdf, 0u, H, xi.x);
    uint col = sampleCdf1D(conditionalCdf, row * (W + 1u), W, xi.y);

    // Continuous inversion within selected bin.
    float rowLo = marginalCdf[row];
    float rowHi = marginalCdf[row + 1u];
    float tRow  = (rowHi > rowLo) ? clamp((xi.x - rowLo) / (rowHi - rowLo), 0.0, 1.0) : 0.5;

    uint  cBase = row * (W + 1u);
    float colLo = conditionalCdf[cBase + col];
    float colHi = conditionalCdf[cBase + col + 1u];
    float tCol  = (colHi > colLo) ? clamp((xi.y - colLo) / (colHi - colLo), 0.0, 1.0) : 0.5;

    float uFrac = (float(col) + tCol) / float(W);
    float vFrac = (float(row) + tRow) / float(H);

    // Solid-angle PDF: p_UV * W * H / (2π² * sinθ)
    float pRow     = rowHi - rowLo;
    float pCol     = colHi - colLo;
    float theta    = kEnvSamplePi * vFrac;
    float sinTheta = max(sin(theta), 1.0e-5);
    pdfOmega = sanitizePdf((pRow * pCol * float(W) * float(H)) / (2.0 * kEnvSamplePi * kEnvSamplePi * sinTheta));

    // Reconstruct direction (matches sampleEnvMap lat-long convention).
    float phi = 2.0 * kEnvSamplePi * (uFrac - 0.5);
    float3 dir;
    dir.y = cos(theta);
    dir.x = sinTheta * cos(phi);
    dir.z = sinTheta * sin(phi);
    return normalize(dir);
}

/// Evaluate the solid-angle PDF for direction `dir` under the env CDF.
/// Used by MIS to weight BSDF-sampled rays that escape to the env map.
float evalEnvImportancePdfDir(
    StructuredBuffer<float> marginalCdf,
    StructuredBuffer<float> conditionalCdf,
    uint W, uint H, float3 dir)
{
    if (W == 0u || H == 0u) {
        return 0.0;
    }

    float3 d = normalize(dir);
    float u = atan2(d.z, d.x) * (0.5 * kEnvSampleInvPi) + 0.5;
    float v = acos(clamp(d.y, -1.0, 1.0)) * kEnvSampleInvPi;

    uint col = min(uint(u * float(W)), W - 1u);
    uint row = min(uint(v * float(H)), H - 1u);

    float pRow = marginalCdf[row + 1u] - marginalCdf[row];
    float pCol = conditionalCdf[row * (W + 1u) + col + 1u]
               - conditionalCdf[row * (W + 1u) + col];

    float theta    = kEnvSamplePi * (float(row) + 0.5) / float(H);
    float sinTheta = max(sin(theta), 1.0e-5);
    return sanitizePdf((pRow * pCol * float(W) * float(H)) / (2.0 * kEnvSamplePi * kEnvSamplePi * sinTheta));
}
