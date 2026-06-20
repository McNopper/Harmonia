[[vk::binding(0, 0)]]
RWTexture2D<float4> gCurrentHdr;

[[vk::binding(1, 0)]]
RWTexture2D<float4> gHistoryHdr;

struct AccumulationPushConstants {
    uint sampleCount;
};

[[vk::push_constant]]
AccumulationPushConstants gAccumulation;

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
    const uint2 pixel = dispatchThreadID.xy;
    uint width = 0u;
    uint height = 0u;
    gCurrentHdr.GetDimensions(width, height);
    if (pixel.x >= width || pixel.y >= height) {
        return;
    }

    const float4 current = gCurrentHdr[pixel];
    if (gAccumulation.sampleCount == 0u) {
        gHistoryHdr[pixel] = current;
        return;
    }

    const float4 previous = gHistoryHdr[pixel];
    const float invSampleCount = 1.0 / float(gAccumulation.sampleCount + 1u);
    gHistoryHdr[pixel] = previous + (current - previous) * invSampleCount;
}
