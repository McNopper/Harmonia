[[vk::binding(0, 0)]]
RWTexture2D<float4> gInputHdr;

[[vk::binding(1, 0)]]
RWTexture2D<float4> gOutputHdr;

[[vk::combinedImageSampler]] [[vk::binding(2, 0)]]
Texture2D<float4> gNormalGuide;
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]]
SamplerState gNormalGuideSampler;

[[vk::combinedImageSampler]] [[vk::binding(3, 0)]]
Texture2D<float> gDepthGuide;
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]]
SamplerState gDepthGuideSampler;

[[vk::binding(4, 0)]]
RWTexture2D<float4> gHistoryHdr;

[[vk::binding(5, 0)]][vk::image_format("rg32f")] RWTexture2D<float2>
    gMotionVectors; // per-pixel (dx, dy) in pixel coords; only valid when hasMotionVectors != 0

// ── A-SVGF (A2): adaptive temporal filtering state ──────────────────────────
// R = temporal gradient (pixel-relative luminance change, range [0,1]);
// G = per-pixel variance estimate (gradient²). Written by the temporal pass and
// smoothed by the gradient à-trous filter passes; only valid when the relevant
// push-constant flag (computeGradient / hasGradientVariance) is set.
[[vk::binding(6, 0)]][vk::image_format("rg32f")] RWTexture2D<float2> gGradientVariance;
// Previous frame's gradient/variance. Read for temporal smoothing of the
// gradient, and reused as the ping-pong source for the gradient filter passes.
[[vk::binding(7, 0)]][vk::image_format("rg32f")] RWTexture2D<float2> gPrevGradientVariance;

// ── DN3-M2: paper-exact converging blend (Eq. 2 / Eq. 14) ────────────────────
// Binding 8 = the ORIGINAL accumulated input n (pre-filter, pre-temporal);
// binding 9 = the perturbed-filter chain output D' (bound by the host to the
// last scratch of the perturbed chain). Read only by the temporal (final
// writer) invocation when convergingMode is set.
[[vk::binding(8, 0)]]
RWTexture2D<float4> gOriginalHdr;

[[vk::binding(9, 0)]]
RWTexture2D<float4> gPerturbedOutput;

struct DenoiserPushConstants {
    float strength;
    float historyBlend;
    uint hasNormalGuide;
    uint hasDepthGuide;
    uint applyHistory;
    uint historyFirstUse;
    uint iterations;
    uint passIndex;
    uint hasMotionVectors; // 1 when gMotionVectors is populated; 0 = static lookup fallback
    // ── A-SVGF (A2) ─────────────────────────────────────────────────────────
    uint computeGradient;     // 1 = temporal pass computes gradient + variance
    uint gradientFilterMode;  // 1 = this dispatch is the gradient à-trous blur pass
    uint gradientFilterPass;  // 0..N: gradient à-trous pass index (tap spacing)
    uint hasGradientVariance; // 1 = gGradientVariance holds valid gradient/variance
    float gradientAlpha;      // temporal blend for the gradient (default 0.2)
    // ── DN3-M2: paper-exact converging blend (Eq. 2 / Eq. 14) ────────────────
    uint convergingMode;  // 1 = blend per paper Eq. 2 with the Eq. 14 per-pixel weight
    uint perturbedMode;   // 1 = this dispatch is the perturbed chain (adds eps*b inline)
    uint sampleCount;     // current accumulated sample count I
    uint prevSampleCount; // previous frame's sample count (Eq. 7 curve second point)
};

[[vk::push_constant]]
DenoiserPushConstants gDenoiser;

float luminance(float3 c) {
    return dot(c, float3(0.2627f, 0.6780f, 0.0593f));
}

// Per-component non-finite scrub: replaces NaN/Inf with 0. NOTE this does NOT
// catch huge-but-finite garbage (e.g. uninitialised device memory reinterpreted
// as a ~1e30 float), so it is only a second line of defence — the temporal path
// below must never read uninitialised history into arithmetic (see the
// first-use seeding branch).
float4 sanitize4(float4 v) {
    return float4(
        isfinite(v.x) ? v.x : 0.0f, isfinite(v.y) ? v.y : 0.0f, isfinite(v.z) ? v.z : 0.0f, isfinite(v.w) ? v.w : 0.0f);
}

// DN3-M2: static per-pixel perturbation in [-1, 1]^3 (position-hashed; the paper
// uses pre-generated Gaussian randoms - a static zero-mean uniform draw keeps
// the first-order SURE approximation usable while needing no upload).
float3 perturbNoise(uint2 p) {
    uint s = p.x * 1973u + p.y * 9277u + 26699u;
    s = s * 747796405u + 2891336453u;
    s = (s ^ (s >> 16u)) * 2654435761u;
    s = (s ^ (s >> 13u)) * 1274126177u;
    const float3 e = float3(float(s & 0xFFu), float((s >> 8u) & 0xFFu), float((s >> 16u) & 0xFFu));
    return e / 127.5f - 1.0f.xxx;
}

[numthreads(8, 8, 1)] void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
    const uint2 pixel = dispatchThreadID.xy;
    uint width = 0u;
    uint height = 0u;
    gInputHdr.GetDimensions(width, height);
    if (pixel.x >= width || pixel.y >= height) {
        return;
    }

    // ── A-SVGF gradient propagation pass (à-trous on the gradient buffer) ────
    // Spreads the temporal-gradient / variance signal across a neighbourhood so
    // an isolated disocclusion/lighting-change pixel influences its surround
    // (disocclusion affects a region, not one texel) and per-pixel gradient
    // noise is removed. Reads the source (gPrevGradientVariance), writes the
    // destination (gGradientVariance); the host swaps the two views per pass so
    // successive iterations ping-pong. Ref: Schied et al., A-SVGF (HPG 2018).
    if (gDenoiser.gradientFilterMode != 0u) {
        const uint gstep = 1u << min(gDenoiser.gradientFilterPass, 4u);
        const float gk[3] = {0.25f, 0.5f, 0.25f};
        float2 gacc = 0.0f.xx;
        float gwsum = 0.0f;
        for (int gy = -1; gy <= 1; ++gy) {
            for (int gx = -1; gx <= 1; ++gx) {
                const int2 gc =
                    clamp(int2(pixel) + int2(gx, gy) * int(gstep), int2(0, 0), int2(int(width) - 1, int(height) - 1));
                const float2 gv = gPrevGradientVariance[uint2(gc)];
                const float gw = gk[gx + 1] * gk[gy + 1];
                gacc += gv * gw;
                gwsum += gw;
            }
        }
        gGradientVariance[pixel] = (gwsum > 1e-6f) ? (gacc / gwsum) : gPrevGradientVariance[pixel];
        return;
    }

    const float2 invExtent = 1.0f / float2(max(width, 1u), max(height, 1u));
    const float2 baseUv = (float2(pixel) + 0.5f.xx) * invExtent;
    float4 center = sanitize4(gInputHdr[pixel]);
    // DN3-M2 perturbed chain pass 0: perturb every read (center here, taps below)
    // with the static hash draw scaled by epsilon = 1 (paper Eq. 13).
    if (gDenoiser.perturbedMode != 0u && gDenoiser.passIndex == 0u) {
        center.xyz += perturbNoise(pixel);
        center = sanitize4(center);
    }

    float4 filtered = center;
    if (gDenoiser.applyHistory == 0u) {
        // ── À-trous wavelet edge-stopping filter (SVGF-style spatial core) ──────
        // Each iteration keeps a fixed 5-tap (B3-spline) kernel per axis but spaces
        // the taps by stepSize = 2^passIndex, so the spatial support grows
        // exponentially across iterations at constant cost. Edge-stopping weights
        // (normal / depth / luminance) preserve geometry while the widening reach
        // suppresses low-frequency Monte-Carlo variance (e.g. glass fireflies).
        // Ref: Dammertz et al., "Edge-Avoiding À-Trous Wavelet Transform for fast
        //      Global Illumination Filtering" (HPG 2010); Schied et al., SVGF (HPG 2017).
        const uint stepSize = 1u << min(gDenoiser.passIndex, 6u);
        // Separable B3-spline kernel weights for tap offsets {-2,-1,0,1,2}.
        const float kKernel[5] = {0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f};

        const float centerLuma = luminance(max(center.xyz, 0.0f.xxx));
        const float3 centerNormal =
            gDenoiser.hasNormalGuide != 0u
                ? normalize(gNormalGuide.SampleLevel(gNormalGuideSampler, baseUv, 0.0f).xyz * 2.0f.xxx - 1.0f.xxx)
                : float3(0.0f, 0.0f, 1.0f);
        const float centerDepth =
            gDenoiser.hasDepthGuide != 0u ? gDepthGuide.SampleLevel(gDepthGuideSampler, baseUv, 0.0f) : 0.0f;

        // Edge-stopping sensitivities. Depth/luminance tolerances are loosened at
        // coarser à-trous levels (divided by stepSize) so wide taps still blend on
        // gentle gradients while sharp geometry edges stay rejected.
        const float normalSigma = 20.0f + gDenoiser.strength * 72.0f;
        const float depthSigma = (16.0f + gDenoiser.strength * 64.0f) / float(stepSize);
        float lumaSigma = (1.5f + (1.0f - gDenoiser.strength) * 8.0f) / float(stepSize);

        // GI2.7: M-aware denoising. The HDR alpha channel carries the ReSTIR reservoir's
        // effective sample count M (written by Theia's GI compute stage). High-M pixels
        // are well-sampled → tighter luma edge stops (less over-smoothing of genuine
        // detail). Low-M pixels are noisy → looser stops (more aggressive denoising).
        // The sqrt scaling follows the standard error-monotonicity of MC integration
        // (error ~ 1/sqrt(M)). Ref: Ouyang et al., "RTXDI" (I3D 2021), M-aware filtering.
        const float centerM = max(center.w, 0.0f);
        const float mScale = saturate(sqrt(centerM / 32.0f)); // normalize to M_ref = 32
        lumaSigma *= max(mScale, 0.3f);                       // floor at 30% so low-M pixels still get filtered

        // ── DN3: converging kernel blend (Denisova & Bocchi, I3D 2024, Eq. 2). ────
        // The fixed-radius à-trous filter destroys structure at a rate that depends
        // on render resolution, not sample count — a denoised render never converges
        // to the true image. DN3's fix: blend filtered toward the input with an
        // analytical weight W/(I+W) where W is the denoised pixel's equivalent sample
        // count from the convergence curve log(v) = a·log(I) + b (Eq. 7).
        // For MC integration, a ≈ -1 (variance ∝ 1/I), so W ≈ I·v_I/v_d where v_I is
        // the noisy variance and v_d the denoised variance. The denoised pixel's
        // effective sample count determines its weight — pixels the denoiser improved
        // a lot get high W (denoised dominates); pixels it barely changed get low W.
        // Provably converging: as I → ∞, W/(I+W) → 0 regardless of v_d, so the output
        // converges to the input (identity) → the true image.
        // Algorithm-agnostic: I is the effective sample count (reservoir M or spp).
        const float kConvRef = 256.0f; // I at which the filter is ~50% blended
        // W ≈ kConvRef (denoiser improves pixel to ~kConvRef-spp quality at I=1).
        // Blend weight W/(I+W) = kConvRef/(M + kConvRef) → 0 as M → ∞.
        const float convBlend = kConvRef / (centerM + kConvRef);

        float4 sum = 0.0f.xxxx;
        float totalWeight = 0.0f;
        for (int y = -2; y <= 2; ++y) {
            for (int x = -2; x <= 2; ++x) {
                const int2 offset = int2(x, y) * int(stepSize);
                const int2 sampleCoordI =
                    clamp(int2(pixel) + offset, int2(0, 0), int2(int(width) - 1, int(height) - 1));
                const uint2 sampleCoord = uint2(sampleCoordI);
                const float2 sampleUv = (float2(sampleCoord) + 0.5f.xx) * invExtent;
                float4 sampleColor = gInputHdr[sampleCoord];
                // DN3-M2 perturbed chain pass 0: tap positions are perturbed too.
                if (gDenoiser.perturbedMode != 0u && gDenoiser.passIndex == 0u) {
                    sampleColor.xyz += perturbNoise(sampleCoord);
                }

                // Skip non-finite neighbours so a single firefly/NaN can never bias
                // the weighted mean (the divide would otherwise propagate it).
                if (!all(isfinite(sampleColor))) {
                    continue;
                }

                const float sampleLuma = luminance(max(sampleColor.xyz, 0.0f.xxx));
                float weight = kKernel[x + 2] * kKernel[y + 2];
                // Variance-guided luma edge stopping (A-SVGF): where the per-pixel
                // variance estimate is high (noisy / recently disoccluded), loosen
                // the luminance threshold so those regions are filtered more
                // aggressively. With hasGradientVariance == 0 this is a no-op
                // (varSigmaBoost = 0 → lumaSigmaAdaptive == lumaSigma).
                const float varSigmaBoost =
                    (gDenoiser.hasGradientVariance != 0u) ? saturate(gGradientVariance[sampleCoord].g * 8.0f) : 0.0f;
                const float lumaSigmaAdaptive = lumaSigma / (1.0f + varSigmaBoost);
                weight *= exp(-abs(sampleLuma - centerLuma) * lumaSigmaAdaptive);

                if (gDenoiser.hasNormalGuide != 0u) {
                    const float3 sampleNormal = normalize(
                        gNormalGuide.SampleLevel(gNormalGuideSampler, sampleUv, 0.0f).xyz * 2.0f.xxx - 1.0f.xxx);
                    const float normalDiff = 1.0f - saturate(dot(centerNormal, sampleNormal));
                    weight *= exp(-normalDiff * normalSigma);
                }
                if (gDenoiser.hasDepthGuide != 0u) {
                    const float sampleDepth = gDepthGuide.SampleLevel(gDepthGuideSampler, sampleUv, 0.0f);
                    weight *= exp(-abs(sampleDepth - centerDepth) * depthSigma);
                }

                sum += sampleColor * weight;
                totalWeight += weight;
            }
        }
        filtered = (totalWeight > 1e-6f) ? (sum / totalWeight) : center;
        filtered = sanitize4(filtered);
        // DN3 (shipped v0.7.9 constant-W blend): converge to identity as M -> inf
        // - blend filtered toward the raw input. In DN3-M2 converging mode this
        // constant-W lerp is skipped: the paper-exact Eq. 2 + Eq. 14 blend in the
        // temporal invocation replaces it (per-pixel SURE weight).
        if (gDenoiser.convergingMode == 0u) {
            filtered = sanitize4(lerp(center, filtered, convBlend));
        }
    }

    if (gDenoiser.applyHistory != 0u) {
        // ── Temporal accumulation (exponential moving average) ─────────────────
        // Ai = lerp(Ci, Ai-1, alpha)  with first-frame seeding and a Karis-style
        // temporal colour clamp (Karis, "High Quality Temporal Supersampling",
        // SIGGRAPH 2014; Schied et al., SVGF, HPG 2017).
        if (gDenoiser.historyFirstUse != 0u) {
            // No valid history yet (the image is UNDEFINED on first use and after a
            // reset, holding uninitialised device memory that may be huge-but-finite
            // garbage which sanitize4() cannot scrub). Seed the accumulation buffer
            // with the current frame instead of reading that memory — even a
            // zero-weight blend risks 0 * Inf = NaN. Output stays = current frame.
            if (gDenoiser.computeGradient != 0u) {
                // Seed the gradient/variance to zero (no temporal change yet) so the
                // next frame's smoothing reads a defined value instead of garbage.
                gGradientVariance[pixel] = float2(0.0f, 0.0f);
            }
            gHistoryHdr[pixel] = filtered;
        } else {
            // Temporal reprojection: when motion vectors are available, look up the
            // history at the reprojected screen position so camera motion does not
            // blend a pixel with a different world point from the previous frame.
            // Without motion vectors (hasMotionVectors == 0), fall back to the
            // original static-pixel lookup (backward-compatible with Hyperion).
            float4 history;
            if (gDenoiser.hasMotionVectors != 0u) {
                float2 mv = gMotionVectors[pixel]; // pixel-coord delta (prev - cur)
                int2 reproj = clamp(int2(pixel) + int2(round(mv)), int2(0, 0), int2(int(width) - 1, int(height) - 1));
                history = sanitize4(gHistoryHdr[reproj]);
                // Disocclusion: very large motion means the sample came from off-screen
                // or was heavily occluded — seed history at the current position instead
                // of blending stale ghosted data.
                float2 absMotion = abs(mv);
                bool disoccluded = absMotion.x > float(width) * 0.5f || absMotion.y > float(height) * 0.5f;
                if (disoccluded) {
                    if (gDenoiser.computeGradient != 0u) {
                        // Full temporal change: max gradient so downstream passes
                        // reject stale history around the disoccluded region.
                        gGradientVariance[pixel] = float2(1.0f, 1.0f);
                    }
                    gHistoryHdr[pixel] = filtered;
                    gOutputHdr[pixel] = sanitize4(filtered);
                    return;
                }
            } else {
                history = sanitize4(gHistoryHdr[pixel]);
            }
            // Temporal colour clamp: bound the history sample to a plausible band
            // around the current pixel so a stale outlier cannot poison the EMA.
            // Per-pixel (1x1) clamp keeps the in-place history pass deterministic
            // (reading a wider neighbourhood would race the concurrent writes); it
            // is a no-op once the history has converged (history ~= current).
            const float3 clampHi = max(filtered.xyz, 0.0f.xxx) * 8.0f + 1.0f.xxx;
            history.xyz = clamp(history.xyz, 0.0f.xxx, clampHi);
            const float blend = saturate(gDenoiser.historyBlend);
            if (gDenoiser.computeGradient != 0u) {
                // ── A-SVGF adaptive temporal gradient + history rejection ─────
                // Temporal gradient: pixel-relative luminance change between the
                // current estimate and the reprojected history (range [0,1]).
                const float curLum = luminance(max(filtered.xyz, 0.0f.xxx));
                const float prevLum = luminance(max(history.xyz, 0.0f.xxx));
                const float gradient = (prevLum > 1e-4f) ? abs(curLum - prevLum) / (curLum + prevLum + 1e-4f) : 0.0f;
                // Smooth the gradient temporally against the previous frame.
                const float prevGrad = gPrevGradientVariance[pixel].r;
                const float smoothGrad = lerp(gradient, prevGrad, gDenoiser.gradientAlpha);
                const float variance = smoothGrad * smoothGrad;
                gGradientVariance[pixel] = float2(smoothGrad, variance);
                // High gradient (disocclusion / lighting change / fast motion)
                // lowers the history weight so the estimate re-converges faster;
                // a stable pixel (gradient → 0) keeps the full configured blend,
                // so a converged static scene is unchanged from the fixed-blend path.
                const float adaptiveBlend = blend * (1.0f - saturate(smoothGrad * 4.0f));
                filtered = sanitize4(lerp(filtered, history, adaptiveBlend));
            } else {
                filtered = sanitize4(lerp(filtered, history, blend));
            }
            gHistoryHdr[pixel] = filtered;

            // ── DN3-M2: paper-exact converging blend (Eq. 2 + Eq. 14) ───────
            // Applied to the presentation output only - the history above keeps
            // accumulating the pre-blend denoised estimate (the paper's n_i(I)
            // stays a fresh MC estimate per iteration). Engages from 16 samples
            // (paper sec. 4: artifacts below; curves stable from 64) and with a
            // valid curve second point (prevSampleCount > 0). Presentation-only:
            // offscreen identity per the v0.7.4 two-tier contract is unchanged.
            if (gDenoiser.convergingMode != 0u && gDenoiser.hasGradientVariance != 0u &&
                gDenoiser.prevSampleCount != 0u && gDenoiser.sampleCount >= 16u) {
                const float4 n4 = sanitize4(gOriginalHdr[pixel]);
                const float4 dPert = sanitize4(gPerturbedOutput[pixel]);
                const float nLum = luminance(max(n4.xyz, 0.0f.xxx));
                const float dLum = luminance(max(filtered.xyz, 0.0f.xxx));
                const float dPertLum = luminance(max(dPert.xyz, 0.0f.xxx));
                const float relVar = max(gGradientVariance[pixel].g, 1.0e-6f);
                const float sigma2 = relVar * max(nLum * nLum, 1.0e-4f);
                // Monte-Carlo SURE first-order approximation (paper Eq. 12 + 13,
                // K = 1, epsilon = 1): divergence of the denoising chain estimated
                // from the perturbed-chain output. Negative SURE -> 0 (paper
                // sec. 2.6, low-count zeroing strategy).
                const float sure =
                    max(-sigma2 + (dLum - nLum) * (dLum - nLum) + 2.0f * sigma2 * (dPertLum - dLum), 0.0f);
                // Per-pixel convergence curve from the last two variance points
                // (paper Eq. 7); fallback a = -1 recovers pure MC scaling.
                const float I = max(float(gDenoiser.sampleCount), 1.0f);
                const float Iprev = max(float(gDenoiser.prevSampleCount), 1.0f);
                const float vCur = max(gGradientVariance[pixel].g, 1.0e-8f);
                const float vPrev = max(gPrevGradientVariance[pixel].g, 1.0e-8f);
                float slopeA = -1.0f;
                float interceptB = log(max(vCur * I, 1.0e-8f));
                if (vCur > 1.0e-8f && vPrev > 1.0e-8f && I > Iprev + 0.5f) {
                    const float la = log(I);
                    const float lp = log(Iprev);
                    slopeA = clamp((log(vCur) - log(vPrev)) / (la - lp), -2.0f, -0.05f);
                    interceptB = log(vCur) - slopeA * la;
                }
                // Weight (paper Eq. 14); SURE is converted to the curve's relative
                // units (the variance signal is pixel-relative luminance squared).
                const float sureRel = sure / max(nLum * nLum, 1.0e-4f);
                float weightW = exp((log(max(sureRel, 1.0e-8f)) - interceptB) / slopeA);
                weightW = clamp(weightW, 0.0f, 1.0e7f);
                const float denom = I + weightW;
                // Eq. 2 blend on RGB; alpha keeps this frame's reservoir M (the
                // M-aware pipeline reads it downstream).
                filtered = (denom > 1.0e-6f) ? sanitize4(float4((I * n4.x + weightW * filtered.x) / denom,
                                                                (I * n4.y + weightW * filtered.y) / denom,
                                                                (I * n4.z + weightW * filtered.z) / denom,
                                                                n4.w))
                                             : filtered;
            }
        }
    }

    gOutputHdr[pixel] = sanitize4(filtered);
}
