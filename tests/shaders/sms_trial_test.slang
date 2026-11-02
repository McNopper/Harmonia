import sms;

struct SmsTrialInput {
    uint4 counter;
    uint4 keyAndMode;
};
struct SmsTrialPush {
    SmsTrialInput* inputs;
    uint4* outputs;
    uint count;
    uint pad;
};
[[vk::push_constant]] SmsTrialPush pc;

uint category(float u) {
    return u < 0.0625 ? 0u : (u < 0.25 ? 1u : 2u);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= pc.count) return;
    SmsTrialInput input = pc.inputs[id.x];
    uint2 key = input.keyAndMode.xy;
    if (input.keyAndMode.z == 0u) {
        pc.outputs[id.x] = smsPhilox(input.counter, key);
        return;
    }
    uint root = category(smsTrialSample(key, uint2(0, 0), uint2(0, 0)).x);
    SmsReciprocal state = beginSmsReciprocal();
    if (root == 2u) {
        pc.outputs[id.x] = uint4(0, 0, 3, root); // Failed initial sample: zero contribution.
        return;
    }
    for (uint trial = 1u; trial <= 4096u && state.status == 0u; ++trial) {
        uint repeated = category(smsTrialSample(key, uint2(trial, 0), uint2(0, 0)).x);
        observeSmsReciprocal(state, repeated == root);
    }
    // An unfinished estimate stays pending and FAILS the host test, never capped.
    pc.outputs[id.x] = uint4(state.trials, state.status, root);
}
