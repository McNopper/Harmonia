// SMS reciprocal-probability accounting (Zeltner 2020, Algorithm 2).
// For ONE fixed root, the caller provides fresh independent trials with the same
// solver/seed law as the initial sample. A trial succeeds only on the SAME root.
// Pausing is not truncation: persist the state, and publish only after completion.
// Root equivalence, time budgets and approximate PDFs are not hidden here.
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

// Philox4x32-10, Salmon et al. 2011, "Parallel Random Numbers: As Easy as 1, 2, 3".
// Algorithm/constants and known answers: DEShawResearch/random123, BSD-3-Clause.
// Copyright 2010-2012, D. E. Shaw Research. All rights reserved.
// See THIRD-PARTY-NOTICES.md. Self-contained cross-vendor Slang adaptation;
// no Random123 library, vendor RNG or shaderInt64 capability is required.
uint2 smsMultiplyWide(uint a, uint b) {
    // Four 16-bit limb products; every intermediate fits in a uint32.
    uint p0 = (a & 0xFFFFu) * (b & 0xFFFFu);
    uint p1 = (a >> 16u) * (b & 0xFFFFu) + (p0 >> 16u);
    uint p2 = (a & 0xFFFFu) * (b >> 16u) + (p1 & 0xFFFFu);
    uint hi = (a >> 16u) * (b >> 16u) + (p1 >> 16u) + (p2 >> 16u);
    return uint2((p2 << 16u) | (p0 & 0xFFFFu), hi);
}

uint4 smsPhilox(uint4 counter, uint2 key) {
    for (uint round = 0u; round < 10u; ++round) {
        uint2 product0 = smsMultiplyWide(0xD2511F53u, counter.x);
        uint2 product1 = smsMultiplyWide(0xCD9E8D57u, counter.z);
        counter = uint4(product1.y ^ counter.y ^ key.x, product1.x,
                        product0.y ^ counter.w ^ key.y, product0.x);
        key += uint2(0x9E3779B9u, 0xBB67AE85u);
    }
    return counter;
}

// trial=0 generates the initial seed; trial=1... generates repeat trials for the
// SAME fixed root/light/caster. sampleBlock separates any further random requests
// within a trial without consuming another trial's counter range. Caller keys
// must distinguish pixel, sample/frame, receiver/depth and candidate.
float4 smsTrialSample(uint2 key, uint2 trial, uint2 sampleBlock) {
    uint4 bits = smsPhilox(uint4(trial, sampleBlock), key);
    // Exactly representable 24-bit uniforms, strictly [0,1); no rounded endpoint 1.
    return float4(bits >> 8u) * (1.0 / 16777216.0);
}
