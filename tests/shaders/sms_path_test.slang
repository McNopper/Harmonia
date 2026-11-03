import common;
import sms_path;
import path_integrator;
struct Push { uint4* inputs; float4* outputs; uint count,pad; };
[[vk::push_constant]] Push pc;
static const uint kSpec=1u,kTransmit=2u,kInside=4u,kScatter=8u,kNull=16u,kTerminated=32u,kReceiver=64u;

GpuMaterial material(uint kind) {
    GpuMaterial value={};
    value.baseColorWeight=1.0.xxxx;
    value.opacityFlagsPad=float4(1,float(kind),0,0);
    value.textureIndices=value.textureIndices2=kNoTexture.xxxx;
    value.specularRoughAnisoIor=float4(0,0,1.5,0);
    value.specularColorWeight=float4(0,0,0,1);
    if (kind==2u) value.transmissionColorWeight=1.0.xxxx;
    return value;
}
void advance(inout SmsPathState path,GpuMaterial mat,uint remaining,uint flags,bool back=false,uint objectId=7u) {
    SurfaceHit surface={}; surface.mat=mat; surface.wo=float3(0,0,1);
    PathState after={};
    after.nextDirection=(flags&kTransmit)!=0u ? float3(0,0,-1) : float3(0,0,1);
    after.specularBounce=(flags&kSpec)!=0u; after.insideMedium=(flags&kInside)!=0u;
    after.mediumAlbedo=(flags&kScatter)!=0u ? .5.xxx : 0.0.xxx;
    after.passedThrough=(flags&kNull)!=0u; after.terminated=(flags&kTerminated)!=0u;
    smsAdvancePathSurface(path,surface,after,back ? float3(0,0,-1) : float3(0,0,1),
        uint2(objectId,1),remaining,(flags&kReceiver)!=0u);
}
bool receive(inout SmsPathState path,uint remaining) {
    GpuMaterial diffuse=material(0u);
    bool captured=smsCapturePathReceiver(path,diffuse,remaining);
    advance(path,diffuse,remaining,captured ? kReceiver : 0u);
    return captured;
}

[shader("compute")]
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x>=pc.count) return;
    uint mode=pc.inputs[id.x].x;
    GpuMaterial mirror=material(3u),glass=material(2u),bulk=material(2u);
    bulk.transmissionParams.x=1.0;
    SmsPathState path=beginSmsPath();
    if (mode==1u) advance(path,mirror,4u,kSpec);
    if (mode==5u) advance(path,mirror,4u,kSpec|kNull);
    if (mode==6u) advance(path,bulk,5u,kSpec|kTransmit|kInside|kScatter);
    if (mode==7u) {
        advance(path,bulk,5u,kSpec|kTransmit|kInside);
        smsAdvancePathMediumBoundary(path,true,uint2(7,1),true,float3(0,0,-1),float3(0,0,-1));
    }
    uint budget=(mode==2u || mode==8u || mode==9u || mode==10u || mode==13u) ? 4u : 3u;
    if (mode==12u) budget=2u;
    bool captured=receive(path,budget);
    if (mode==4u) captured=receive(path,3u);
    if (mode==2u || mode==3u || mode==10u || mode==13u) {
        if (mode==13u) glass.baseColorWeight.w=5e-7;
        advance(path,glass,budget-1u,kSpec|kTransmit);
        advance(path,glass,2u,kSpec|kTransmit,true,mode==10u ? 8u : 7u);
    } else if (mode==8u || mode==9u) {
        advance(path,bulk,3u,kSpec|kTransmit|kInside);
        if (mode==9u) smsAdvancePathMediumBoundary(path,false,uint2(7,1),true,float3(0,0,-1),float3(0,0,-1));
        smsAdvancePathMediumBoundary(path,true,uint2(7,1),true,float3(0,0,-1),float3(0,0,-1));
    } else {
        uint flags=kSpec|(mode==14u?kTerminated:0u)|(mode==15u?kNull:0u);
        advance(path,mirror,2u,flags);
        if (mode==11u) advance(path,mirror,1u,kSpec);
    }
    pc.outputs[id.x]=float4(smsPathEmissionTail(path)?1.0:0.0,float(path.kind),
        path.prefixSpecular?1.0:0.0,captured?1.0:0.0);
}
