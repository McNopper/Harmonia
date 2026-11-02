import common;
import math;
import bsdf;
import sms_transport;
struct Probe { float4 settings; uint4 modes; };
struct ProbePush { Probe* inputs; float4* outputs; uint count, pad; };
[[vk::push_constant]] ProbePush pc;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= pc.count) return;
    Probe input = pc.inputs[id.x];
    GpuMaterial mat = {};
    mat.baseColorWeight = float4(1,1,1,input.settings.x);
    mat.specularColorWeight = float4(1,1,1,1);
    mat.specularRoughAnisoIor = float4(0,0,1.5,0);
    mat.transmissionColorWeight = float4(1,1,1,1);
    mat.textureIndices = mat.textureIndices2 = kNoTexture.xxxx;
    mat.opacityFlagsPad = float4(1,input.modes.x == 1u ? 3.0 : 2.0,0,0);
    if (input.modes.z != 0u) {
        mat.transmissionParams.x = input.settings.z;
        mat.transmissionColorWeight.xyz = float3(.6,.8,.9);
        if (input.modes.w != 0u) mat.transmissionScatter.xyz = .1.xxx;
    }
    if (input.modes.x == 2u) {
        mat.emissionColorLum = float4(1,1,1,1);
        GpuEmissiveTriangle light = {};
        light.v0_area = float4(0,0,0,input.settings.x);
        light.edge1_emitR = float4(2,0,0,1);
        light.edge2_emitG = float4(0,1,0,1);
        light.normal_emitB = float4(0,0,1,1);
        float mass = smsEmitterSelectionMass(input.settings.w,input.settings.z);
        bool eligible = smsProposableEmitter(light,mat,mass);
        pc.outputs[id.x] = float4(eligible ? 1.0 : 0.0,mass,
            eligible ? mass/smsEmitterArea(light) : 0.0,0.0);
        return;
    }
    float c = input.settings.y;
    float3 wo = float3(sqrt(max(0.0,1.0-c*c)),0,c);
    float3 n = float3(0,0,1), t = float3(1,0,0), b = float3(0,1,0);
    RngState rng = makeRngState(123u,0u);
    BSDFSample sampled = sampleBSDF(mat,wo,n,t,b,n,t,b,false,rng);
    float3 response = smsDeltaResponse(mat,wo,n,input.modes.y,1.5);
    pc.outputs[id.x] = float4(smsReflectionCaster(mat) ? 1.0 : 0.0,
        response.y,sampled.weight.y,sampled.pdf);
}
