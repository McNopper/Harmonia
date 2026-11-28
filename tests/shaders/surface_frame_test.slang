import common;
import math;
import surface;

/// A texture set of constant colours: slot i returns colors[i].
struct ConstantTextures : ISurfaceTextureSampler {
    float4 colors[8];
    float4 sampleTex(uint idx, float2 uv) { return colors[idx]; }
};

struct FramePush { float4* outputs; };
[[vk::push_constant]] FramePush pc;

/// Per mode, six float4: normal, T, B, coatN, coatT, coatB of the frame that
/// applyOpenPbrTextureMaps builds. All maps are unbound unless a mode binds one.
///
///  0  normal +Z, texture tangent +X
///  1  normal +Z, texture tangent +Y
///  2  normal +Z, texture tangent between +X and +Y
///  3  a tilted normal, texture tangent +Y
///  4  texture tangent parallel to the normal (a degenerate projection)
///  5  no texture tangent
///  6  a coat normal map tilting the coat normal away from the base normal; texture tangent +Y
///  7  a base tangent map storing the +v direction; the coat has no tangent map
[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint mode=id.x;
    if (mode>=11u) return;
    GpuMaterial mat={};
    mat.textureIndices=uint4(kNoTexture,kNoTexture,kNoTexture,kNoTexture);
    mat.textureIndices2=uint4(kNoTexture,kNoTexture,kNoTexture,kNoTexture);
    mat.textureIndices3=uint4(kNoTexture,kNoTexture,kNoTexture,kNoTexture);
    mat.textureIndices4=uint4(kNoTexture,kNoTexture,kNoTexture,kNoTexture);
    ConstantTextures tex={};
    float3 n=float3(0,0,1);
    float3 tangent=float3(1,0,0);
    if (mode==1u) tangent=float3(0,1,0);
    if (mode==2u) tangent=normalize(float3(1,1,0));
    if (mode==3u) { n=normalize(float3(0,.6,.8)); tangent=float3(0,1,0); }
    if (mode==4u) tangent=float3(0,0,1);
    if (mode==5u) tangent=float3(0,0,0);
    if (mode==6u) { mat.textureIndices2.x=0u; tex.colors[0]=float4(.5,1,1,1); tangent=float3(0,1,0); }
    if (mode==7u) { mat.textureIndices2.y=0u; tex.colors[0]=float4(.5,1,0,1); }
    if (mode==8u || mode==9u) mat.emissionColorLum=float4(1,1,1,2);
    SurfaceFrame f=applyOpenPbrTextureMaps(mat,tex,n,n,n,tangent,float2(.5,.5));
    pc.outputs[mode*6u+0u]=float4(f.normal,0);
    if (mode>=8u) {
        float3 wo=mode==9u ? -n : n;
        pc.outputs[mode*6u+0u]=float4(hasPositiveFrontFaceEmission(mat,wo,n) ? 1 : 0,0,0,0);
    }
    pc.outputs[mode*6u+1u]=float4(f.T,0);
    pc.outputs[mode*6u+2u]=float4(f.B,0);
    pc.outputs[mode*6u+3u]=float4(f.coatN,0);
    pc.outputs[mode*6u+4u]=float4(f.coatT,0);
    pc.outputs[mode*6u+5u]=float4(f.coatB,0);
}
