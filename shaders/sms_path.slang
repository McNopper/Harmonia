// Actual-event PT/SMS partition, independent of whether any root solve succeeds.
// First eligible opaque receiver only; later non-delta vertices stay PT-owned.
__exported import sms_transport;
import common;
import path_integrator;
import medium_walk;

struct SmsPathState {
    uint kind;
    uint2 casterKey;
    bool prefixSpecular;
};

SmsPathState makeSmsPathState(uint kind,uint2 casterKey,bool prefixSpecular) {
    SmsPathState state;
    state.kind=kind; state.casterKey=casterKey; state.prefixSpecular=prefixSpecular;
    return state;
}
SmsPathState beginSmsPath() { return makeSmsPathState(1u,uint2(0,0),true); }

bool smsCapturePathReceiver(inout SmsPathState path,GpuMaterial material,uint remainingDepth) {
    if (!path.prefixSpecular || remainingDepth<3u || !smsOpaqueReceiver(material)) return false;
    path.kind=2u;
    return true;
}

bool smsPathEmissionTail(SmsPathState path) { return path.kind==3u || path.kind==5u; }

void smsAdvancePathSurface(inout SmsPathState path,SurfaceHit surface,PathState after,
    float3 rawGeoNormal,uint2 objectKey,uint remainingDepth,bool currentReceiver) {
    uint previous=path.kind,next=0u;
    bool front=dot(surface.wo,rawGeoNormal)>0.0;
    bool reflected=dot(surface.wo,rawGeoNormal)*dot(after.nextDirection,rawGeoNormal)>0.0;
    GpuMaterial material=surface.mat;
    if (!after.terminated && !after.passedThrough && after.insideMedium && previous==2u &&
        after.specularBounce && front && !reflected && smsSmoothGlass(material) && smsPureAbsorption(material) &&
        mediumIsPureAbsorber(after.mediumAlbedo) && remainingDepth>=3u) {
        next=4u; path.casterKey=objectKey;
    } else if (!after.terminated && !after.passedThrough && !after.insideMedium) {
        if (currentReceiver) next=2u;
        else if (previous==2u && after.specularBounce && front && smsReflectionCaster(material)) {
            if (reflected) next=3u;
            else if (smsSmoothGlass(material) && remainingDepth>=3u) { next=4u; path.casterKey=objectKey; }
        } else if (previous==4u && after.specularBounce && !front && !reflected &&
            all(objectKey==path.casterKey) && smsSmoothGlass(material)) next=5u;
    }
    path.kind=next;
    if (after.passedThrough || !after.specularBounce ||
        (after.insideMedium && !mediumIsPureAbsorber(after.mediumAlbedo))) path.prefixSpecular=false;
}

void smsAdvancePathMediumBoundary(inout SmsPathState path,bool exited,uint2 objectKey,
    bool pureAbsorption,float3 direction,float3 rawOutwardNormal) {
    if (path.kind==4u)
        path.kind=exited && all(objectKey==path.casterKey) && pureAbsorption &&
            dot(direction,rawOutwardNormal)>0.0 ? 5u : 0u;
}

bool smsCollectablePathEmitter(uint count,StructuredBuffer<GpuEmissiveTriangle> emitters,
    StructuredBuffer<float> cdf,GpuMaterial material,uint2 objectKey,uint primitive,
    float3 wo,float3 rawGeoNormal) {
    if (!smsEmitterMaterial(material) || dot(wo,rawGeoNormal)<=0.0) return false;
    for (uint i=0u;i<count;++i) {
        GpuEmissiveTriangle light=emitters[i];
        float mass=smsEmitterSelectionMass(cdf[i],i==0u ? 0.0 : cdf[i-1u]);
        if (smsProposableEmitter(light,material,mass) && light.kind_pad.y==objectKey.y &&
            light.kind_pad.z==objectKey.x && light.kind_pad.w==primitive) return true;
    }
    return false;
}
