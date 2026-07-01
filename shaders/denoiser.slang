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

[[vk::binding(5, 0)]] [vk::image_format("rg32f")]
RWTexture2D<float2> gMotionVectors;  // per-pixel (dx, dy) in pixel coords; only valid when hasMotionVectors != 0

struct DenoiserPushConstants {
    float strength;
    float historyBlend;
    uint hasNormalGuide;
    uint hasDepthGuide;
    uint applyHistory;
    uint historyFirstUse;
    uint iterations;
    uint passIndex;
    uint hasMotionVectors;  // 1 when gMotionVectors is populated; 0 = static lookup fallback
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
    return float4(isfinite(v.x) ? v.x : 0.0f,
                  isfinite(v.y) ? v.y : 0.0f,
                  isfinite(v.z) ? v.z : 0.0f,
                  isfinite(v.w) ? v.w : 0.0f);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
    const uint2 pixel = dispatchThreadID.xy;
    uint width = 0u;
    uint height = 0u;
    gInputHdr.GetDimensions(width, height);
    if (pixel.x >= width || pixel.y >= height) {
        return;
    }

    const float2 invExtent = 1.0f / float2(max(width, 1u), max(height, 1u));
    const float2 baseUv = (float2(pixel) + 0.5f.xx) * invExtent;
    const float4 center = sanitize4(gInputHdr[pixel]);

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
        const float3 centerNormal = gDenoiser.hasNormalGuide != 0u
            ? normalize(gNormalGuide.SampleLevel(gNormalGuideSampler, baseUv, 0.0f).xyz * 2.0f.xxx - 1.0f.xxx)
            : float3(0.0f, 0.0f, 1.0f);
        const float centerDepth = gDenoiser.hasDepthGuide != 0u
            ? gDepthGuide.SampleLevel(gDepthGuideSampler, baseUv, 0.0f)
            : 0.0f;

        // Edge-stopping sensitivities. Depth/luminance tolerances are loosened at
        // coarser à-trous levels (divided by stepSize) so wide taps still blend on
        // gentle gradients while sharp geometry edges stay rejected.
        const float normalSigma = 20.0f + gDenoiser.strength * 72.0f;
        const float depthSigma = (16.0f + gDenoiser.strength * 64.0f) / float(stepSize);
        const float lumaSigma = (1.5f + (1.0f - gDenoiser.strength) * 8.0f) / float(stepSize);

        float4 sum = 0.0f.xxxx;
        float totalWeight = 0.0f;
        for (int y = -2; y <= 2; ++y) {
            for (int x = -2; x <= 2; ++x) {
                const int2 offset = int2(x, y) * int(stepSize);
                const int2 sampleCoordI =
                    clamp(int2(pixel) + offset, int2(0, 0), int2(int(width) - 1, int(height) - 1));
                const uint2 sampleCoord = uint2(sampleCoordI);
                const float2 sampleUv = (float2(sampleCoord) + 0.5f.xx) * invExtent;
                const float4 sampleColor = gInputHdr[sampleCoord];

                // Skip non-finite neighbours so a single firefly/NaN can never bias
                // the weighted mean (the divide would otherwise propagate it).
                if (!all(isfinite(sampleColor))) {
                    continue;
                }

                const float sampleLuma = luminance(max(sampleColor.xyz, 0.0f.xxx));
                float weight = kKernel[x + 2] * kKernel[y + 2];
                weight *= exp(-abs(sampleLuma - centerLuma) * lumaSigma);

                if (gDenoiser.hasNormalGuide != 0u) {
                    const float3 sampleNormal =
                        normalize(gNormalGuide.SampleLevel(gNormalGuideSampler, sampleUv, 0.0f).xyz * 2.0f.xxx - 1.0f.xxx);
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
            gHistoryHdr[pixel] = filtered;
        } else {
            // Temporal reprojection: when motion vectors are available, look up the
            // history at the reprojected screen position so camera motion does not
            // blend a pixel with a different world point from the previous frame.
            // Without motion vectors (hasMotionVectors == 0), fall back to the
            // original static-pixel lookup (backward-compatible with Hyperion).
            float4 history;
            if (gDenoiser.hasMotionVectors != 0u) {
                float2 mv = gMotionVectors[pixel];  // pixel-coord delta (prev - cur)
                int2 reproj = clamp(int2(pixel) + int2(round(mv)),
                                    int2(0, 0), int2(int(width)-1, int(height)-1));
                history = sanitize4(gHistoryHdr[reproj]);
                // Disocclusion: very large motion means the sample came from off-screen
                // or was heavily occluded — seed history at the current position instead
                // of blending stale ghosted data.
                float2 absMotion = abs(mv);
                bool disoccluded = absMotion.x > float(width) * 0.5f ||
                                   absMotion.y > float(height) * 0.5f;
                if (disoccluded) {
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
            filtered = sanitize4(lerp(filtered, history, blend));
            gHistoryHdr[pixel] = filtered;
        }
    }

    gOutputHdr[pixel] = sanitize4(filtered);
}
