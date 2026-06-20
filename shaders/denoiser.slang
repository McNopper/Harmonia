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

struct DenoiserPushConstants {
    float strength;
    float historyBlend;
    uint hasNormalGuide;
    uint hasDepthGuide;
    uint applyHistory;
    uint historyFirstUse;
    uint iterations;
    uint passIndex;
};

[[vk::push_constant]]
DenoiserPushConstants gDenoiser;

float luminance(float3 c) {
    return dot(c, float3(0.2627f, 0.6780f, 0.0593f));
}

// Per-component non-finite scrub. The history image is transitioned from UNDEFINED on
// its first use and never explicitly cleared, so on the seeding frame gHistoryHdr holds
// uninitialized device memory that may contain NaN/Inf bit patterns. lerp() propagates a
// NaN history forever (NaN blends to NaN), poisoning the accumulated denoise and the
// captured EXR. Scrubbing to 0 makes the first-use seed deterministic and is a no-op once
// the history holds real (finite) radiance.
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
    const float4 center = gInputHdr[pixel];

    float4 filtered = center;
    if (gDenoiser.applyHistory == 0u) {
        const int radius = int(1u + min(uint(round(gDenoiser.strength * 2.0f)), 2u));
        const float centerLuma = luminance(max(center.xyz, 0.0f.xxx));
        const float3 centerNormal = gDenoiser.hasNormalGuide != 0u
            ? normalize(gNormalGuide.SampleLevel(gNormalGuideSampler, baseUv, 0.0f).xyz * 2.0f.xxx - 1.0f.xxx)
            : float3(0.0f, 0.0f, 1.0f);
        const float centerDepth = gDenoiser.hasDepthGuide != 0u
            ? gDepthGuide.SampleLevel(gDepthGuideSampler, baseUv, 0.0f)
            : 0.0f;

        float4 sum = 0.0f.xxxx;
        float totalWeight = 0.0f;
        const float normalSigma = 20.0f + gDenoiser.strength * 72.0f;
        const float depthSigma = 16.0f + gDenoiser.strength * 64.0f;
        const float lumaSigma = 1.5f + (1.0f - gDenoiser.strength) * 8.0f;
        const float spatialSigma = 0.15f + (1.0f - gDenoiser.strength) * 0.5f;

        for (int y = -radius; y <= radius; ++y) {
            for (int x = -radius; x <= radius; ++x) {
                const int2 sampleCoordI = clamp(int2(pixel) + int2(x, y), int2(0, 0), int2(int(width) - 1, int(height) - 1));
                const uint2 sampleCoord = uint2(sampleCoordI);
                const float2 sampleUv = (float2(sampleCoord) + 0.5f.xx) * invExtent;
                const float4 sampleColor = gInputHdr[sampleCoord];
                const float sampleLuma = luminance(max(sampleColor.xyz, 0.0f.xxx));

                const float dist2 = float(x * x + y * y);
                float weight = exp(-dist2 * spatialSigma);
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
    }

    if (gDenoiser.applyHistory != 0u) {
        const float4 history = sanitize4(gHistoryHdr[pixel]);
        const float blend = (gDenoiser.historyFirstUse != 0u) ? 1.0f : saturate(gDenoiser.historyBlend);
        filtered = lerp(filtered, history, blend);
        gHistoryHdr[pixel] = filtered;
    }

    gOutputHdr[pixel] = filtered;
}
