import sms;

struct SmsReciprocalPush {
    uint4* inputs;  // failures before match, chunk size, initial low/high count
    uint4* outputs;
    uint count;
    uint pad;
};
[[vk::push_constant]] SmsReciprocalPush pc;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= pc.count) return;
    uint4 input = pc.inputs[id.x];
    SmsReciprocal state = beginSmsReciprocal();
    state.trials = input.zw;
    uint invalidPublication = 0u;
    uint observed = 0u;
    float weight = 0.0;
    // Test inputs are small deterministic sequences, not random unbounded loops.
    while (observed <= input.x) {
        uint chunk = min(input.y, input.x + 1u - observed);
        for (uint j = 0u; j < chunk; ++j) {
            observeSmsReciprocal(state, observed == input.x);
            ++observed;
        }
        bool ready = completedSmsReciprocal(state, weight);
        if (observed <= input.x && ready) invalidPublication = 1u;
    }
    pc.outputs[id.x] = uint4(state.trials, state.status,
        invalidPublication != 0u ? 0xFFFFFFFFu : asuint(weight));
}
