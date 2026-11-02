import common;
import math;
import bsdf;

struct MeasurePush { uint4* inputs; float4* outputs; uint count, pad; };
[[vk::push_constant]] MeasurePush pc;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= pc.count) return;
    uint mode = pc.inputs[id.x].x;
    GpuMaterial mat = {};
    mat.baseColorWeight = float4(1, 1, 1, 1);
    mat.specularColorWeight = float4(1, 1, 1, 1);
    mat.specularRoughAnisoIor = float4(mode == 1u || mode == 3u || mode == 8u || mode == 11u ? 0.2 : 0.0, 0, 1.5, 0);
    if (mode == 5u) mat.specularRoughAnisoIor.x = 0.05;
    if (mode == 6u) mat.specularRoughAnisoIor.x = 0.049;
    if (mode == 7u) mat.transmissionParams.z = 1.0;
    mat.transmissionColorWeight = float4(1, 1, 1, 1);
    mat.coatColorWeight = float4(1, 1, 1, mode == 4u || mode == 9u ? 0.5 : 0.0);
    mat.coatRoughAnisoIorDark = float4(0.3, 0, 1.6, 0);
    mat.opacityFlagsPad = float4(1, mode == 2u ? 3.0 : 2.0, 0,
        mode == 3u || mode == 8u || mode == 11u ? 1.0 : 0.0);
    if (mode == 8u) {
        mat.opacityFlagsPad.y = 0.0;
        mat.transmissionColorWeight.w = 0.4;
        mat.subsurfaceColorWeight = float4(1, 1, 1, 0.5);
    }
    if (mode == 2u) {
        mat.baseMetalnessDiffRough.x = 1.0;
        mat.transmissionColorWeight.w = 0.0;
    }
    float3 n = float3(0, 0, 1), t = float3(1, 0, 0), b = float3(0, 1, 0);
    float3 wo = mode >= 10u ? float3(sqrt(0.75), 0, 0.5) : n;
    float3 wi = mode == 2u || mode == 4u || mode == 9u ? n : -n;
    if (mode == 10u) wi = reflect(-wo, n);
    float3 coatN = mode == 9u ? normalize(float3(0, 1, 1)) : n;
    float3 coatB = mode == 9u ? normalize(float3(0, 1, -1)) : b;
    bool exiting = mode >= 10u;
    float3 value = evalBSDF(mat, wo, wi, n, t, b, coatN, t, coatB, exiting);
    float pdf = pdfBSDF(mat, wo, wi, n, t, b, coatN, t, coatB, exiting);
    RngState rng = makeRngState(123u, 0u);
    BSDFSample sampled = sampleBSDF(mat, wo, n, t, b, coatN, t, coatB, exiting, rng);
    pc.outputs[id.x] = float4(value.y, pdf, sampled.isDelta ? 1.0 : 0.0, sampled.pdf);
}
