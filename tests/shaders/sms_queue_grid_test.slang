import sms_queue;
struct GridPush { uint4* inputs; uint4* outputs; uint count, pad; };
[[vk::push_constant]] GridPush pc;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= pc.count) return;
    uint4 input = pc.inputs[id.x];
    SmsQueueHeader header = {};
    header.count = header.capacity = input.x;
    smsQueueGrid(header);
    uint index;
    bool valid = smsQueueIndex(header, uint3(input.yz, 0), input.w, index);
    pc.outputs[id.x] = uint4(header.dispatchX, header.dispatchY, valid ? 1u : 0u, index);
}
