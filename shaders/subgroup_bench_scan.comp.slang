// VK4 micro-benchmark — subgroup arm: compaction via a subgroup prefix sum
// (WavePrefixSum) plus one per-subgroup global base reservation, replacing the
// per-thread atomic of subgroup_bench_atomic.comp.slang.
//
// The subgroup size is assumed to be 32 (verified min=max=32 on the reference GPU
// via vulkaninfo); a device with a different fixed subgroup size would still be
// CORRECT (subIndex = tid / 32 only partitions the groupshared base slots) but the
// benchmark pairs one base slot per 32 lanes, so extra lanes in a >32 subgroup
// share a slot — outputs remain identical because each slot's reservation is the
// sum of that partition's predicates.
//
// Validity contract: identical output to subgroup_bench_atomic.comp.slang.

[[vk::binding(0, 0)]]
StructuredBuffer<uint> gInput;

[[vk::binding(1, 0)]]
RWStructuredBuffer<uint> gOutput;

[[vk::binding(2, 0)]]
RWStructuredBuffer<uint> gCount;

groupshared uint gs_subBase[8]; // 256 threads / 32 lanes = 8 subgroups per group

[numthreads(256, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID, uint3 groupThreadID : SV_GroupThreadID) {
    const uint value = gInput[dispatchThreadID.x];
    const uint predicate = (value != 0u) ? 1u : 0u;
    const uint lane = WaveGetLaneIndex();
    const uint subIndex = groupThreadID.x / 32u; // WITHIN the workgroup — gs_subBase has 8 slots
    const uint exclusive = WavePrefixSum(predicate);
    const uint subTotal = WaveActiveSum(predicate);

    if (lane == 0u) {
        uint base;
        InterlockedAdd(gCount[0], subTotal, base);
        gs_subBase[subIndex] = base;
    }
    GroupMemoryBarrierWithGroupSync();

    if (predicate == 1u) {
        gOutput[gs_subBase[subIndex] + exclusive] = value;
    }
}
