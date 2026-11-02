// SMS reciprocal-probability accounting (Zeltner 2020, Algorithm 2).
// For ONE fixed root, the caller provides fresh independent trials with the same
// solver/seed law as the initial sample. A trial succeeds only on the SAME root.
// Pausing is not truncation: persist the state, and publish only after completion.
// No RNG, root-equivalence rule, time budget or approximate PDF is hidden here.
struct SmsReciprocal {
    uint2 trials; // low/high words, to avoid requiring shaderInt64
    uint status; // 0 pending, 1 complete, 2 counter overflow (fatal to the experiment)
};

SmsReciprocal beginSmsReciprocal() {
    SmsReciprocal state;
    state.trials = uint2(0u, 0u);
    state.status = 0u;
    return state;
}

void observeSmsReciprocal(inout SmsReciprocal state, bool sameRoot) {
    if (state.status != 0u) return;
    state.trials.x += 1u;
    if (state.trials.x == 0u) {
        state.trials.y += 1u;
        if (state.trials.y == 0u) {
            state.status = 2u;
            return;
        }
    }
    if (sameRoot) state.status = 1u;
}

bool completedSmsReciprocal(SmsReciprocal state, out float weight) {
    weight = 0.0;
    if (state.status != 1u) return false;
    weight = float(state.trials.y) * 4294967296.0 + float(state.trials.x);
    return true;
}
