import sms_queue;
[[vk::push_constant]] SmsQueuePush pc;

[shader("compute")]
[numthreads(1, 1, 1)]
void main() {
    if (pc.operation == 3u) {
        // Serial promotion after validation: no mixed atomic/non-atomic status
        // or error reads race the full-slot validator's atomic error writes.
        if (pc.inputHeader->error != 0u) {
            pc.inputHeader->status = kSmsQueueFailed;
            pc.inputHeader->count = 0u;
            pc.inputHeader->dispatchX = 0u;
        }
        return;
    }
    if (pc.operation == 0u) {
        SmsQueueHeader initial = {};
        initial.count = pc.capacity;
        initial.capacity = pc.capacity;
        initial.status = pc.capacity == 0u ? kSmsQueueReady : kSmsQueuePending;
        smsQueueGrid(initial);
        uint2 dimensions=uint2(0,0);
        if (pc.userFlags!=0u && pc.capacity!=0u) dimensions=pc.work[0].words[15].zw;
        initial.reserved[1]=uint4(dimensions.x/16u+(dimensions.x%16u!=0u?1u:0u),
            dimensions.y/16u+(dimensions.y%16u!=0u?1u:0u),1u,0u);
        *pc.inputHeader = initial;
    }
    if (pc.operation <= 1u) {
        SmsQueueHeader output = {};
        output.mode = 1u;
        output.capacity = pc.capacity;
        output.status = kSmsQueuePending;
        output.error = pc.inputHeader->error;
        SmsQueueHeader validation = {};
        validation.count = pc.capacity;
        smsQueueGrid(validation);
        output.reserved[0] = uint4(validation.dispatchX, validation.dispatchY, validation.dispatchZ, pc.capacity);
        output.reserved[1] = pc.inputHeader->reserved[1];
        *pc.outputHeader = output;
    } else {
        SmsQueueHeader output = *pc.outputHeader;
        output.error |= pc.inputHeader->error;
        if (output.count > pc.capacity) output.error |= kSmsQueueOverflow;
        output.status = output.error != 0u ? kSmsQueueFailed :
            (output.count == 0u ? kSmsQueueReady : kSmsQueuePending);
        if (output.error != 0u) output.count = 0u;
        smsQueueGrid(output);
        *pc.outputHeader = output;
    }
}
