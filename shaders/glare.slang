// glare.slang — lens glare of the display path.
//
// The light a lens scatters from the bright parts of the frame. Three compute
// entry points share one descriptor layout (push descriptors):
//   binding 0: the source image
//   binding 1: the coarser copy (the upsample only)
//   binding 2: the target image
// and one push-constant block. All images are in GENERAL and are read with
// Load at texel centres, so no sampler takes part.
//
// prefilter:   the frame (full resolution) -> level 0 (half resolution). A
//              13-tap downsample filter (Jimenez, "Next Generation Post
//              Processing in Call of Duty: Advanced Warfare", SIGGRAPH 2014),
//              every texel of it first reduced to what lies above a soft knee
//              around the threshold.
// downsample:  level i -> level i+1 with the same 13-tap filter.
// upsample:    level i of the upsampled chain = level i of the downsampled
//              chain + weight x (level i+1 of the upsampled chain under a
//              tent filter two texels wide). With a weight of 0 it copies.
//
// Both filters preserve a constant and its integral. So the glare of a small
// bright object spreads over levels of doubling width with a weight that
// falls geometrically, and a frame that lies below the threshold everywhere
// has no glare.

struct GlarePushConstants {
    float2 sourceTexel;  // 1 / size of the source image; for the upsample, of the coarser copy
    float2 targetTexel;  // 1 / size of the target image
    float  threshold;    // the prefilter: scene-linear luminance above which a texel's light is scattered
    float  weight;       // the upsample: share of the coarser copy
};

[[vk::push_constant]] GlarePushConstants pc;

[[vk::binding(0, 0)]] RWTexture2D<float4> t_source;
[[vk::binding(1, 0)]] RWTexture2D<float4> t_coarse;
[[vk::binding(2, 0)]] RWTexture2D<float4> t_target;

/// The size an image declares through its texel size (1 / size).
int2 sizeOf(float2 texel) {
    return max(int2(floor(1.0 / texel + 0.5)), int2(1, 1));
}

/// A texel read, clamped to the image so that the filters stay normalized at
/// its border. Negative and non-finite values are not light.
float4 texel(RWTexture2D<float4> image, int2 coordinate, int2 size) {
    const int2 inside = clamp(coordinate, int2(0, 0), size - 1);
    float4 value = image[inside];
    if (!all(isfinite(value))) {
        value = 0.0.xxxx;
    }
    return max(value, 0.0.xxxx);
}

/// What a texel contributes to the glare: the part of its luminance above the
/// threshold, with a soft knee below it. The knee is half a bell as wide as
/// half the threshold: the contribution rises quadratically from (threshold
/// - knee) to the threshold and linearly above it.
float4 brightPass(float4 color) {
    const float threshold = pc.threshold;
    const float knee = 0.5 * threshold;
    const float luminance = dot(color.xyz, float3(0.2126, 0.7152, 0.0722));
    float soft = clamp(luminance - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    const float contribution = max(soft, luminance - threshold) / max(luminance, 1.0e-5);
    return color * contribution;
}

/// The mean of the 2x2 source texels whose top-left one is `corner`.
float4 block(int2 corner, int2 size, bool bright) {
    const int2 right = corner + int2(1, 0);
    const int2 down = corner + int2(0, 1);
    const int2 diagonal = corner + int2(1, 1);
    float4 sum = bright ? brightPass(texel(t_source, corner, size)) : texel(t_source, corner, size);
    sum += bright ? brightPass(texel(t_source, right, size)) : texel(t_source, right, size);
    sum += bright ? brightPass(texel(t_source, down, size)) : texel(t_source, down, size);
    sum += bright ? brightPass(texel(t_source, diagonal, size)) : texel(t_source, diagonal, size);
    return 0.25 * sum;
}

/// The 13-tap filter for a destination texel: the 2x2 source block it covers
/// (weight 1/8), the four blocks diagonal of it (1/32 each), the four blocks
/// beside it (1/16 each) and the four 2x2 groups across its corners (1/8
/// each). The weights sum to 1.
float4 downsample13(int2 destination, int2 size, bool bright) {
    const int2 base = destination * 2;
    float4 sum = 0.125 * block(base, size, bright);
    sum += 0.03125 * (block(base + int2(-2, -2), size, bright) + block(base + int2(2, -2), size, bright) +
                      block(base + int2(-2, 2), size, bright) + block(base + int2(2, 2), size, bright));
    sum += 0.0625 * (block(base + int2(0, -2), size, bright) + block(base + int2(-2, 0), size, bright) +
                     block(base + int2(2, 0), size, bright) + block(base + int2(0, 2), size, bright));
    sum += 0.125 * (block(base + int2(-1, 1), size, bright) + block(base + int2(1, 1), size, bright) +
                    block(base + int2(-1, -1), size, bright) + block(base + int2(1, -1), size, bright));
    return sum;
}

/// The coarse image under a tent filter two texels wide, at the position of a
/// destination texel's centre. Weights: (1 - |d| / 2) / 2 per axis, d the
/// distance to a coarse texel's centre; they sum to 1 for every position, and
/// only the four texels from floor(centre) - 1 on carry weight.
float4 tent(int2 destination, int2 coarseSize, int2 destinationSize) {
    const float2 ratio = float2(coarseSize) / float2(destinationSize);
    const float2 centre = (float2(destination) + 0.5) * ratio - 0.5;
    const int2 base = int2(floor(centre));
    const float2 offset = centre - float2(base);
    float4 sum = 0.0.xxxx;
    for (int dy = -1; dy <= 2; ++dy) {
        const float wy = max(0.0, 1.0 - 0.5 * abs(float(dy) - offset.y));
        for (int dx = -1; dx <= 2; ++dx) {
            const float wx = max(0.0, 1.0 - 0.5 * abs(float(dx) - offset.x));
            sum += 0.25 * wx * wy * texel(t_coarse, base + int2(dx, dy), coarseSize);
        }
    }
    return sum;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void prefilterMain(uint3 tid : SV_DispatchThreadID) {
    const int2 destination = int2(tid.xy);
    if (any(destination >= sizeOf(pc.targetTexel))) {
        return;
    }
    t_target[destination] = downsample13(destination, sizeOf(pc.sourceTexel), true);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void downsampleMain(uint3 tid : SV_DispatchThreadID) {
    const int2 destination = int2(tid.xy);
    if (any(destination >= sizeOf(pc.targetTexel))) {
        return;
    }
    t_target[destination] = downsample13(destination, sizeOf(pc.sourceTexel), false);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void upsampleMain(uint3 tid : SV_DispatchThreadID) {
    const int2 destination = int2(tid.xy);
    const int2 destinationSize = sizeOf(pc.targetTexel);
    if (any(destination >= destinationSize)) {
        return;
    }
    float4 sum = texel(t_source, destination, destinationSize);
    const float weight = pc.weight;
    if (weight > 0.0) {
        sum += weight * tent(destination, sizeOf(pc.sourceTexel), destinationSize);
    }
    t_target[destination] = sum;
}
