import common;
import sms;
import sms_partition;
struct Input { uint4 grid; float4 random; };
struct Push { Input* inputs; float4* outputs; uint count,pad; };
[[vk::push_constant]] Push pc;

[shader("compute")]
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x>=pc.count) return;
    Input input=pc.inputs[id.x];
    uint n=input.grid.x;
    uint2 center=input.grid.yz;
    uint width=smsRegionWidth(n);
    float probability=smsRegionProbability(n);
    if (input.grid.w==4u) {
        uint2 a=uint2(n/2u-1u,n/2u),b=uint2(n/2u,n/2u);
        float phi=2.0*kPi*(float(n/2u)+.5)/float(n);
        SmsConditionalRegion region=makeSmsConditionalRegion(normalize(float3(cos(phi),sin(phi),1e-6)),n);
        probability=region.probability;
        uint counter=0u;
        float weightSum=0.0;
        for (uint observation=0;observation<16384u;++observation) {
            uint trials=0u;
            for (;;) {
                float4 u=smsTrialSample(uint2(n,456u),uint2(counter++,0),uint2(0,0));
                float2 seed=smsSampleTileRectangle(u.xy,region.origin,region.extent,n);
                uint2 tile=smsPrimaryTile(seed,n);
                uint2 returned=((tile.x+tile.y)%2u)==0u ? a : b;
                ++trials;
                if (smsRegionContains(returned,tile,n)) break;
            }
            // Independent enumeration: 8/256 initial cells produce this one
            // physical root, with two tolerance-equivalent numeric outputs.
            weightSum+=(8.0/float(n*n))*smsConditionalWeight(float(trials),probability);
        }
        pc.outputs[id.x]=float4(weightSum/16384.0,0,probability,0);
        return;
    }
    if (input.grid.w==5u) {
        float3 direction=normalize(input.random.xyz);
        SmsConditionalRegion region=makeSmsConditionalRegion(direction,n);
        uint missed=0u;
        for (int x=-1;x<=1;++x) for (int y=-1;y<=1;++y) for (int z=-1;z<=1;++z) {
            float3 candidate=normalize(direction+float3(x,y,z)*(kSmsRootDirectionTolerance*.5));
            uint2 tile=smsDirectionTile(candidate,n);
            for (int dx=-1;dx<=1;++dx) for (int dy=-1;dy<=1;++dy) {
                uint2 seed=uint2((int(tile.x)+dx+int(n))%int(n),(int(tile.y)+dy+int(n))%int(n));
                if (!smsConditionalContains(seed,region,n)) ++missed;
            }
        }
        pc.outputs[id.x]=float4(float(missed),region.probability,float2(region.extent));
        return;
    }
    if (input.grid.w==2u) {
        // Known finite-domain solver: root iff seed is in C(root) and an
        // independent uniform is <1/4. Thus P(root)=P(C)/4 and P(root|C)=1/4.
        uint counter=0u,outside=0u;
        float weightSum=0.0,trialSum=0.0;
        for (uint observation=0;observation<16384u;++observation) {
            uint trials=0u;
            for (;;) {
                float4 u=smsTrialSample(uint2(n,123u),uint2(counter++,0),uint2(0,0));
                float2 seed=smsSampleTileRectangle(u.xy,(center+n-1u)%n,width,n);
                bool support=smsRegionContains(smsPrimaryTile(seed,n),center,n);
                if (!support) ++outside;
                ++trials;
                if (support && u.z<.25) break;
            }
            weightSum+=(.25*probability)*smsConditionalWeight(float(trials),probability);
            trialSum+=float(trials);
        }
        pc.outputs[id.x]=float4(weightSum/16384.0,float(outside),probability,trialSum/16384.0);
        return;
    }
    if (input.grid.w==3u) {
        pc.outputs[id.x]=float4(smsTrustContains(0.0.xxx,input.random.xyz,center,n)?1.0:0.0,
            probability,0,0);
        return;
    }
    uint2 origin=input.grid.w==0u ? uint2(0,0) : (center+n-1u)%n;
    uint span=input.grid.w==0u ? n : width;
    float2 seed=smsSampleTileRectangle(input.random.xy,origin,span,n);
    pc.outputs[id.x]=float4(seed,probability,
        smsRegionContains(smsPrimaryTile(seed,n),center,n)?1.0:0.0);
}
