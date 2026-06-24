import common;
import math;
import env_sample;

/// Sample the equirectangular IBL env map for the given direction (Rec.2020 linear).
float3 sampleEnvMap(float3 dir)
{
    return max(envMap.SampleLevel(envSampler, dirToLatlong(dir), 0).xyz, 0.0.xxx);
}

/// Evaluate environment radiance for a given direction (Rec.2020 linear, cd/m²).
/// Returns envLuminanceScale * IBL sample when an env map is loaded,
/// or black (0,0,0) for scenes without an env map.
float3 evalEnvironment(float3 direction, float envLuminanceScale)
{
    if (pushConst.hasEnvMap != 0u) {
        return envLuminanceScale * sampleEnvMap(direction);
    }
    return 0.0.xxx;
}

// ── Env map importance sampling via 2D separable CDF ──────────────────────────
// Pure sampling math lives in env_sample.slang. These wrappers pass the
// renderer's global CDF buffers and push-constant dimensions.
// Ref: Pharr, Jakob & Humphreys — "Physically Based Rendering" 4th ed. §12.5
//      Veach — "Robust Monte Carlo Methods" §9.2 (MIS balance heuristic)

/// Importance-sample the env map using the pre-built 2D separable CDF.
/// Returns the sampled world-space direction; pdfOmega is the solid-angle PDF.
float3 sampleEnvImportance(inout uint seed, out float pdfOmega)
{
    return sampleEnvImportanceDir(
        envMarginalCdf, envConditionalCdf,
        pushConst.envImportanceWidth, pushConst.envImportanceHeight,
        float2(randFloat(seed), randFloat(seed)),
        pdfOmega);
}

/// Evaluate the solid-angle PDF for a given direction under the env importance distribution.
/// Used by the miss shader to compute the MIS weight for BSDF-sampled rays.
float evalEnvImportancePdf(float3 dir)
{
    return evalEnvImportancePdfDir(
        envMarginalCdf, envConditionalCdf,
        pushConst.envImportanceWidth, pushConst.envImportanceHeight,
        dir);
}
