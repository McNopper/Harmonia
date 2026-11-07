// VK5 probe kernel (VK_KHR_pipeline_binary re-investigation): reads an input
// element, writes value + 1. The pipeline-binary probe dispatches this from a
// pipeline CREATED FROM A CAPTURED BINARY and verifies the output — proving the
// consume path produces a working pipeline, not just a valid handle.

[[vk::binding(0, 0)]]
StructuredBuffer<uint> gInput;

[[vk::binding(1, 0)]]
RWStructuredBuffer<uint> gOutput;

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
    gOutput[dispatchThreadID.x] = gInput[dispatchThreadID.x] + 1u;
}
