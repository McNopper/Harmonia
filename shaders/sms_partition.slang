// Hong et al. 2025, Section 3: periodic 3x3 tile trust regions in primary space.
// Directional parameterization is permitted by Section 3.1 / footnote 3.
import common;
import math;

static const float kSmsRootDirectionTolerance=1.0e-4;
struct SmsConditionalRegion { uint2 origin,extent; float probability; };

uint smsRegionWidth(uint resolution) { return min(3u,resolution); }
float smsRegionProbability(uint resolution) {
    uint width=smsRegionWidth(resolution);
    return float(width*width)/float(resolution*resolution);
}
uint2 smsPrimaryTile(float2 u,uint resolution) {
    return min(uint2(u*float(resolution)),(resolution-1u).xx);
}
float2 smsDirectionCoordinates(float3 direction) {
    float x=saturate(.5*(1.0-direction.z));
    float y=direction.x==0.0 && direction.y==0.0 ? 0.0 : atan2(direction.y,direction.x)/(2.0*kPi);
    y-=floor(y);
    return float2(x,y);
}
uint2 smsDirectionTile(float3 direction,uint resolution) {
    float2 u=smsDirectionCoordinates(direction);
    return uint2(min(uint(u.x*resolution),resolution-1u),uint(u.y*resolution)%resolution);
}
bool smsRegionContains(uint2 tile,uint2 center,uint resolution) {
    if (resolution<=3u) return true;
    uint2 distance=(tile+resolution-center)%resolution;
    return (distance.x<=1u || distance.x>=resolution-1u) &&
        (distance.y<=1u || distance.y>=resolution-1u);
}

float smsConditionalWeight(float trials,float regionProbability) { return trials/regionProbability; }

SmsConditionalRegion makeSmsConditionalRegion(float3 direction,uint n) {
    SmsConditionalRegion region;
    region.origin=uint2(0,0);
    region.extent=n.xx;
    region.probability=1.0;
    if (n<=3u) return region;
    // Cover every returned direction that sameSmsRoot may accept, plus the
    // one-cell WALK neighborhood of each possible returned tile. The 2x bound
    // leaves a conservative float-normalization margin without changing matching.
    float delta=2.0*kSmsRootDirectionTolerance;
    float2 u=smsDirectionCoordinates(direction);
    uint lo=min(uint(saturate(u.x-delta*.5)*n),n-1u);
    uint hi=min(uint(saturate(u.x+delta*.5)*n),n-1u);
    region.extent.x=min(hi-lo+3u,n);
    if (region.extent.x<n) region.origin.x=(lo+n-1u)%n;
    float radius=length(direction.xy);
    if (radius>delta) {
        float angle=asin(min(1.0,delta/radius))/(2.0*kPi);
        int first=int(floor((u.y-angle)*n)),last=int(floor((u.y+angle)*n));
        region.extent.y=min(uint(last-first+3),n);
        if (region.extent.y<n) region.origin.y=uint((first-1+2*int(n))%int(n));
    }
    region.probability=float(region.extent.x*region.extent.y)/float(n*n);
    return region;
}
bool smsConditionalContains(uint2 tile,SmsConditionalRegion region,uint n) {
    uint2 distance=(tile+n-region.origin)%n;
    return all(distance<region.extent);
}
bool smsTrustContains(float3 receiver,float3 first,uint2 seedTile,uint resolution) {
    if (resolution<=3u) return true;
    float3 edge=first-receiver;
    float distance=length(edge);
    return distance>0.0 && isfinite(distance) &&
        smsRegionContains(smsDirectionTile(edge/distance,resolution),seedTile,resolution);
}

// The SAME affine cell mapping is used for global and conditional seeds. Global
// uses origin=0,width=N; repeats use origin=rootTile-1,width=min(3,N).
float2 smsSampleTileRectangle(float2 u,uint2 origin,uint2 extent,uint resolution) {
    float2 scaled=u*float2(extent);
    uint2 offset=min(uint2(scaled),extent-1u.xx);
    float2 local=scaled-float2(offset);
    uint2 tile=(origin+offset)%resolution;
    float2 result=(float2(tile)+local)/float(resolution);
    float2 upper=float2(tile+1u)/float(resolution);
    // Keep rounded upper endpoints inside their selected half-open tile. This
    // canonicalizes sampling coordinates; it is not an estimator-weight clamp.
    return min(result,asfloat(asuint(upper)-1u.xx));
}
float2 smsSampleTileRectangle(float2 u,uint2 origin,uint width,uint resolution) {
    return smsSampleTileRectangle(u,origin,width.xx,resolution);
}
