import common;
import sms;
import sms_prior;
struct Push { uint4* inputs; float4* outputs; uint count,pad; };
[[vk::push_constant]] Push pc;

[shader("compute")]
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x>=pc.count) return;
    uint4 input=pc.inputs[id.x];
    uint n=input.x,pattern=input.y,mode=input.z,target=input.w;
    SmsPriorEntry prior={};
    prior.key=1u;
    for (uint index=0u;index<n*n;++index) {
        bool important=pattern==1u || (pattern==2u && (index==0u || index==(n/2u)*n+n/2u)) ||
            (pattern==3u && ((index%n+index/n)%2u)==0u);
        if (important) prior.mask[index/32u]|=1u<<(index%32u);
    }
    SmsConditionalRegion region;
    region.origin=uint2(0,0); region.extent=n.xx; region.probability=1.0;
    if (mode==1u) {
        region.origin=(n-1u).xx; region.extent=smsRegionWidth(n).xx;
    } else if (mode==2u) {
        region.origin=uint2((n/2u+2u*n-2u)%n,(n/2u+n-1u)%n);
        region.extent=uint2(min(4u,n),min(3u,n));
    }
    region.probability=float(region.extent.x*region.extent.y)/float(n*n);
    float2 probabilities=smsPriorRegionProbabilities(prior,region,n);
    uint hits=0u,invalid=0u;
    for (uint observation=0u;observation<65536u;++observation) {
        float4 u=smsTrialSample(uint2(id.x+123u,n),uint2(observation,0),uint2(0,0));
        float2 seed; float mass;
        bool sampled=smsSamplePrior(u,prior,region,n,seed,mass);
        uint2 tile=smsPrimaryTile(seed,n);
        if (!sampled || !smsConditionalContains(tile,region,n)) ++invalid;
        if (tile.y*n+tile.x==target) ++hits;
    }
    pc.outputs[id.x]=float4(probabilities,float(hits)/65536.0,float(invalid));
}
