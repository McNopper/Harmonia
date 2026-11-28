// Test shader: samples one mip level of a 2D texture at texel centres, one texel per
// thread, into a float4 buffer. Nearest sampling applies the same block and sRGB decoding
// as the production texture path, without filtering between texels or mip levels. The
// extent of the level travels in the push constants.

[[vk::binding(0, 0)]] Sampler2D g_source;
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> g_texels;

struct ReadbackPush
{
    uint level;
    uint width;
    uint height;
};

[[vk::push_constant]] ReadbackPush pc;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pc.width || tid.y >= pc.height)
    {
        return;
    }
    float2 uv = (float2(tid.xy) + 0.5) / float2(pc.width, pc.height);
    g_texels[tid.y * pc.width + tid.x] = g_source.SampleLevel(uv, float(pc.level));
}
