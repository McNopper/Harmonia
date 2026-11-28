import common;
import math;
import bsdf;

struct MeasurePush { float4* outputs; };
[[vk::push_constant]] MeasurePush pc;
[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint mode=id.x;
    if (mode>=30u) return;
    if (mode==25u || mode==26u) {
        // Full evalBSDF, not just weights: base_weight=0 removes the metal closure. At metalness
        // 0.5 the surviving dielectric closure must be exactly half of the pure-dielectric reference.
        GpuMaterial metalMix={};
        metalMix.baseColorWeight=float4(0,0,0,0);
        metalMix.baseMetalnessDiffRough=float4(mode==25u ? .5 : 0,0,0,0);
        metalMix.specularColorWeight=float4(1,1,1,1);
        metalMix.specularRoughAnisoIor=float4(.2,0,1.5,0);
        metalMix.transmissionColorWeight=float4(1,1,1,0);
        metalMix.coatColorWeight=float4(1,1,1,0);
        metalMix.fuzzColorWeight=float4(1,1,1,0);
        metalMix.opacityFlagsPad=float4(1,0,0,0);
        float3 n=float3(0,0,1),t=float3(1,0,0),b=float3(0,1,0);
        float3 value=evalBSDF(metalMix,n,n,n,t,b,n,t,b,false);
        pc.outputs[mode]=float4(value,1);
        return;
    }
    if (mode>=27u) {
        // Conditional value of actual delta transmission samples. Fractional smooth (27) and
        // thin-walled (28) materials have specular=1, transmission=.5, total=1.5. Their selected
        // sample must carry total*(1-F)=1.44 at normal incidence. Full smooth glass (29) keeps
        // its Fresnel-split ownership and conditional value one. The count proves real samples.
        GpuMaterial deltaMat={};
        deltaMat.baseColorWeight=float4(1,1,1,0);
        deltaMat.specularColorWeight=float4(1,1,1,1);
        deltaMat.specularRoughAnisoIor=float4(mode==28u ? .2 : 0,0,1.5,0);
        deltaMat.transmissionColorWeight=float4(1,1,1,mode==29u ? 1 : .5);
        deltaMat.transmissionParams=float4(0,0,0,0);
        deltaMat.coatColorWeight=float4(1,1,1,0);
        deltaMat.fuzzColorWeight=float4(1,1,1,0);
        deltaMat.opacityFlagsPad=float4(1,0,0,mode==28u ? 1 : 0);
        float3 n=float3(0,0,1),t=float3(1,0,0),b=float3(0,1,0);
        float sum=0;
        uint count=0u;
        for(uint attempt=0u;attempt<64u;++attempt){
            RngState rng=makeRngState(123u,attempt);
            BSDFSample sampled=sampleBSDF(deltaMat,n,n,t,b,n,t,b,false,rng);
            if(sampled.direction.z<0 && sampled.weight.y>0){sum+=sampled.weight.y;++count;}
        }
        pc.outputs[mode]=float4(count ? sum/count : 0,count,sum,0);
        return;
    }
    if (mode>=18u) {
        // OpenPBR 1.1.2 reduction to lobes: the dielectric reflection sits above the whole
        // dielectric substrate; transmission mixes only the opaque and translucent substrates.
        // Modes 19/20 carry the legacy full-glass flag on purpose: a fractional authored weight
        // must override that stale host classification. Mode 21 has no flag: full transmission
        // is recognized from the resolved material, including a textured weight reaching one.
        // Mode 21 is smooth full glass; mode 24 is rough full glass and must retain its GGX reflection.
        GpuMaterial layered={};
        layered.baseColorWeight=float4(1,1,1,mode==23u ? 0 : 1);
        layered.baseMetalnessDiffRough=float4(mode==22u ? .25 : 0,0,0,0);
        layered.specularColorWeight=float4(1,1,1,1);
        layered.specularRoughAnisoIor=float4(mode==21u ? 0 : .2,0,1.5,0);
        layered.transmissionColorWeight=float4(1,1,1,
            mode==18u ? .25 : (mode==19u || mode==22u || mode==23u ? .5 : (mode==20u ? .75 : 1)));
        layered.subsurfaceColorWeight=float4(1,1,1,.5);
        layered.coatColorWeight=float4(1,1,1,0);
        layered.fuzzColorWeight=float4(1,1,1,0);
        layered.opacityFlagsPad=float4(1,mode==19u || mode==20u ? 2 : 0,0,0);
        float diffuseWeight,subsurfaceWeight,specularWeight,metalWeight,transmissionWeight;
        float coatWeight,fuzzWeight,diffuseTransWeight;
        bool mirrorMode,glassMode;
        computeLobeWeights(layered,diffuseWeight,subsurfaceWeight,specularWeight,metalWeight,
            transmissionWeight,coatWeight,fuzzWeight,diffuseTransWeight,mirrorMode,glassMode);
        pc.outputs[mode]=float4(diffuseWeight,subsurfaceWeight,specularWeight,transmissionWeight);
        return;
    }
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
