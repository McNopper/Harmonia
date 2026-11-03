// Hong et al. 2025 Section 4, Eqs. 7--9. A prior is only a frozen tile union;
// its construction never contributes radiance or estimates reciprocal weights.
__exported import sms_partition;

static const uint kSmsPriorSlots=128u;
static const uint kSmsPriorMaskWords=32u; // maximum 32x32 primary-space tiles
static const float kSmsPriorAlpha=.8;
struct SmsPriorEntry { uint key,reserved; uint mask[32]; }; // physical stride 136B

uint smsPriorWord(SmsPriorEntry prior,uint word,uint n) {
    uint first=word*32u,total=n*n;
    if (first>=total) return 0u;
    uint remaining=total-first;
    return remaining>=32u ? prior.mask[word] : prior.mask[word]&((1u<<remaining)-1u);
}
uint smsPriorCount(SmsPriorEntry prior,uint n) {
    uint count=0u;
    for (uint w=0u;w<kSmsPriorMaskWords;++w) count+=countbits(smsPriorWord(prior,w,n));
    return count;
}
bool smsPriorImportant(SmsPriorEntry prior,uint2 tile,uint n) {
    uint index=tile.y*n+tile.x;
    return (prior.mask[index/32u]&(1u<<(index%32u)))!=0u;
}
uint smsPriorIntersection(SmsPriorEntry prior,SmsConditionalRegion region,uint n) {
    if (all(region.extent==n.xx)) return smsPriorCount(prior,n);
    uint count=0u;
    for (uint y=0u;y<region.extent.y;++y) for (uint x=0u;x<region.extent.x;++x)
        if (smsPriorImportant(prior,(region.origin+uint2(x,y))%n,n)) ++count;
    return count;
}
float2 smsPriorRegionProbabilities(SmsPriorEntry prior,SmsConditionalRegion region,uint n) {
    uint total=smsPriorCount(prior,n);
    if (total==0u || total==n*n) return float2(region.probability,0.0);
    uint intersection=smsPriorIntersection(prior,region,n);
    float importantMass=kSmsPriorAlpha*float(intersection)/float(total);
    float mass=importantMass+(1.0-kSmsPriorAlpha)*region.probability;
    return float2(mass,importantMass/mass);
}
uint2 smsPriorRank(SmsPriorEntry prior,SmsConditionalRegion region,uint n,uint rank) {
    if (all(region.extent==n.xx)) {
        for (uint w=0u;w<kSmsPriorMaskWords;++w) {
            uint bits=smsPriorWord(prior,w,n),count=countbits(bits);
            if (rank>=count) { rank-=count; continue; }
            while (rank!=0u) { bits&=bits-1u; --rank; }
            uint index=w*32u+uint(firstbitlow(bits));
            return uint2(index%n,index/n);
        }
    } else {
        for (uint y=0u;y<region.extent.y;++y) for (uint x=0u;x<region.extent.x;++x) {
            uint2 tile=(region.origin+uint2(x,y))%n;
            if (smsPriorImportant(prior,tile,n)) {
                if (rank==0u) return tile;
                --rank;
            }
        }
    }
    return n.xx; // explicit invalid sentinel; caller fails the logical frame
}
bool smsSamplePrior(float4 u,SmsPriorEntry prior,SmsConditionalRegion region,uint n,
    out float2 seed,out float mass) {
    float2 probabilities=smsPriorRegionProbabilities(prior,region,n);
    mass=probabilities.x;
    if (!(mass>0.0) || !isfinite(mass)) { seed=0.0.xx; return false; }
    if (u.w<probabilities.y) {
        uint count=smsPriorIntersection(prior,region,n);
        if (count==0u) { seed=0.0.xx; return false; }
        float scaled=u.x*float(count);
        uint rank=min(uint(scaled),count-1u);
        uint2 tile=smsPriorRank(prior,region,n,rank);
        if (any(tile>=n.xx)) { seed=0.0.xx; return false; }
        seed=smsSampleTileRectangle(float2(scaled-float(rank),u.y),tile,uint2(1,1),n);
    } else seed=smsSampleTileRectangle(u.xy,region.origin,region.extent,n);
    return true;
}

uint smsPriorHash(uint key) { return (key*0x9E3779B9u)&(kSmsPriorSlots-1u); }
SmsPriorEntry smsFindPrior(SmsPriorEntry* table,uint block,uint light) {
    uint key=light+1u,start=smsPriorHash(key);
    for (uint offset=0u;offset<kSmsPriorSlots;++offset) {
        SmsPriorEntry entry=table[block*kSmsPriorSlots+(start+offset)%kSmsPriorSlots];
        if (entry.key==key) return entry;
        if (entry.key==0u) break;
    }
    SmsPriorEntry empty={};
    return empty;
}
