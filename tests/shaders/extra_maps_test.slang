import common;
import math;
import surface;

struct ConstantTextures : ISurfaceTextureSampler {
    float4 colors[8];
    float4 sampleTex(uint idx, float2 uv) { return colors[idx]; }
};

struct ExtraPush { float4* outputs; };
[[vk::push_constant]] ExtraPush pc;

// Mode 0 binds all eight new maps; mode 1 leaves every map unbound as the control.
[shader("compute")]
[numthreads(2, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint mode = id.x;
    if (mode >= 2u) return;
    GpuMaterial mat = {};
    mat.textureIndices = uint4(kNoTexture, kNoTexture, kNoTexture, kNoTexture);
    mat.textureIndices2 = uint4(kNoTexture, kNoTexture, kNoTexture, kNoTexture);
    mat.textureIndices3 = uint4(kNoTexture, kNoTexture, kNoTexture, kNoTexture);
    mat.textureIndices4 = uint4(kNoTexture, kNoTexture, kNoTexture, kNoTexture);
    mat.coatColorWeight = float4(1.0, 1.0, 1.0, 0.0);
    mat.transmissionColorWeight = float4(1.0, 1.0, 1.0, 0.0);
    mat.subsurfaceColorWeight = float4(0.8, 0.8, 0.8, 0.0);
    mat.emissionColorLum = float4(1.0, 1.0, 1.0, 0.5);
    mat.fuzzColorWeight = float4(1.0, 1.0, 1.0, 0.0);
    mat.baseColorWeight = float4(0.8, 0.8, 0.8, 1.0);
    ConstantTextures tex = {};
    if (mode == 0u) {
        mat.textureIndices3 = uint4(0u, 1u, 2u, 3u);
        mat.textureIndices4 = uint4(4u, 5u, 6u, 7u);
        tex.colors[0] = float4(0.25, 0.5, 0.75, 1.0);
        tex.colors[1] = float4(0.6, 0.0, 0.0, 1.0);
        tex.colors[2] = float4(0.2, 0.4, 0.6, 1.0);
        tex.colors[3] = float4(0.35, 0.0, 0.0, 1.0);
        tex.colors[4] = float4(0.8, 0.1, 0.2, 1.0);
        tex.colors[5] = float4(0.3, 0.5, 0.7, 1.0);
        tex.colors[6] = float4(0.9, 0.0, 0.0, 1.0);
        tex.colors[7] = float4(0.45, 0.0, 0.0, 1.0);
    }
    float3 n = float3(0.0, 0.0, 1.0);
    float3 tangent = float3(1.0, 0.0, 0.0);
    applyOpenPbrTextureMaps(mat, tex, n, n, n, tangent, float2(0.5, 0.5));
    pc.outputs[mode * 8u + 0u] = mat.coatColorWeight;
    pc.outputs[mode * 8u + 1u] = mat.transmissionColorWeight;
    pc.outputs[mode * 8u + 2u] = mat.subsurfaceColorWeight;
    pc.outputs[mode * 8u + 3u] = mat.transmissionScatter;
    pc.outputs[mode * 8u + 4u] = mat.emissionColorLum;
    pc.outputs[mode * 8u + 5u] = mat.fuzzColorWeight;
    pc.outputs[mode * 8u + 6u] = mat.baseColorWeight;
    pc.outputs[mode * 8u + 7u] = mat.baseMetalnessDiffRough;
}
