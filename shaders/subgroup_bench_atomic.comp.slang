// VK4 micro-benchmark — baseline arm: compaction via a per-thread atomic counter.
// Validity contract: identical output to subgroup_bench_scan.comp.slang for the same
// input (the test asserts equality; timings are logged, never gated — a shared dev
// machine has no stable perf baseline).

[[vk::binding(0, 0)]]
StructuredBuffer<uint> gInput;

[[vk::binding(1, 0)]]
RWStructuredBuffer<uint> gOutput;

[[vk::binding(2, 0)]]
RWStructuredBuffer<uint> gCount;

[numthreads(256, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
    const uint value = gInput[dispatchThreadID.x];
    if (value != 0u) {
        uint index;
        InterlockedAdd(gCount[0], 1u, index);
        gOutput[index] = value;
    }
}
