import sms_queue;
[[vk::push_constant]] SmsQueuePush pc;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex) {
    uint slot;
    if (!smsQueueSlot(pc, group, lane, slot)) return;
    uint4 state = pc.work[slot].words[0];
    uint ignored;
    uint ledger = pc.inputHeader->mode == 0u ? 0u : (state.x == 2u ? 1u : 2u);
    InterlockedAdd(pc.work[slot].words[1][ledger], 1u, ignored);
    if (state.z == 1u || (state.z == 4u && pc.inputHeader->mode == 1u)) {
        smsQueueError(pc, kSmsQueueNumericError);
    } else if (state.z == 3u && pc.inputHeader->mode == 0u) {
        // Corrupt producer metadata, then exercise the consumer's invalid-index path.
        uint index;
        InterlockedAdd(pc.outputHeader->count, 1u, index);
        if (index < pc.capacity) pc.outputIndices[index] = pc.capacity;
        else smsQueueError(pc, kSmsQueueOverflow);
    } else if (state.z == 2u) {
        // Deliberately faulty producer: exceeds one append per slot. The queue
        // must report overflow without writing outside its index allocation.
        smsQueueAppend(pc, slot);
        smsQueueAppend(pc, slot);
    } else if (pc.inputHeader->mode == 0u) {
        if (state.x != 0u) smsQueueAppend(pc, slot); // frozen preparation only
    } else if (state.x != 0u) {
        state.x--;
        if (state.x != 0u) smsQueueAppend(pc, slot);
    }
    pc.work[slot].words[0] = state;
}
