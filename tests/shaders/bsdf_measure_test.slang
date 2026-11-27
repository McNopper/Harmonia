import common;
import math;
import bsdf;

struct MeasurePush { float4* outputs; };
[[vk::push_constant]] MeasurePush pc;
[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint mode=id.x;
    if (mode>=18u) return;
    if (mode>=12u) {
        // 12..17: the direction in which anisotropy stretches the specular highlight. The light
        // leaves the mirror direction (the normal, at normal incidence) by 15 degrees, toward +X
        // (12, 14, 16) or toward +Y (13, 15, 17): at anisotropy 0.9 (12, 13) and at 0 (14, 15)
        // with T = +X; then with the frame turned, T = +Y (16, 17). Base specular alone: the
        // base weight (baseColorWeight.w) keeps the specular lobe, a black base colour removes
        // the diffuse one.
        GpuMaterial an={};
        an.baseColorWeight=float4(0,0,0,1);
        an.specularColorWeight=float4(1,1,1,1);
        an.specularRoughAnisoIor=float4(.3, mode==14u || mode==15u ? 0 : .9, 1.5, 0);
        an.opacityFlagsPad=float4(1,0,0,0);
        float3 n=float3(0,0,1);
        float3 t=mode==16u || mode==17u ? float3(0,1,0) : float3(1,0,0);
        float3 b=cross(n,t);
        float a=radians(15.0);
        float3 wi=mode==12u || mode==14u || mode==16u ? float3(sin(a),0,cos(a)) : float3(0,sin(a),cos(a));
        float3 value=evalBSDF(an,n,wi,n,t,b,n,t,b,false);
        pc.outputs[mode]=float4(value.y,0,0,0);
        return;
    }
    GpuMaterial mat={};
    mat.baseColorWeight=float4(1,1,1,1);
    mat.specularColorWeight=float4(1,1,1,1);
    mat.specularRoughAnisoIor=float4(mode==1u || mode==3u || mode==8u || mode==11u ? .2 : 0,0,1.5,0);
    if (mode==5u) mat.specularRoughAnisoIor.x=.05;
    if (mode==6u) mat.specularRoughAnisoIor.x=.049;
    if (mode==7u) mat.transmissionParams.z=1;
    mat.transmissionColorWeight=float4(1,1,1,1);
    mat.coatColorWeight=float4(1,1,1,mode==4u || mode==9u ? .5 : 0);
    mat.coatRoughAnisoIorDark=float4(.3,0,1.6,0);
    mat.opacityFlagsPad=float4(1,mode==2u ? 3 : 2,0,mode==3u || mode==8u || mode==11u ? 1 : 0);
    if (mode==8u) {
        mat.opacityFlagsPad.y=0;
        mat.transmissionColorWeight.w=.4;
        mat.subsurfaceColorWeight=float4(1,1,1,.5);
    }
    if (mode==2u) { mat.baseMetalnessDiffRough.x=1; mat.transmissionColorWeight.w=0; }
    float3 n=float3(0,0,1),t=float3(1,0,0),b=float3(0,1,0);
    float3 wo=mode>=10u ? float3(sqrt(.75),0,.5) : n;
    float3 wi=mode==2u || mode==4u || mode==9u ? n : -n;
    if (mode==10u) wi=reflect(-wo,n);
    float3 coatN=mode==9u ? normalize(float3(0,1,1)) : n;
    float3 coatB=mode==9u ? normalize(float3(0,1,-1)) : b;
    float3 value=evalBSDF(mat,wo,wi,n,t,b,coatN,t,coatB,mode>=10u);
    float pdf=pdfBSDF(mat,wo,wi,n,t,b,coatN,t,coatB,mode>=10u);
    RngState rng=makeRngState(123u,0u);
    BSDFSample sampled=sampleBSDF(mat,wo,n,t,b,coatN,t,coatB,mode>=10u,rng);
    pc.outputs[mode]=float4(value.y,pdf,sampled.isDelta ? 1 : 0,sampled.pdf);
}
