import sms_queue;
[[vk::push_constant]] SmsQueuePush pc;

[shader("compute")]
[numthreads(1, 1, 1)]
void main() {
    if (pc.operation == 0u) {
        SmsQueueHeader initial = {};
        initial.count = pc.capacity;
        initial.capacity = pc.capacity;
        initial.status = pc.capacity == 0u ? kSmsQueueReady : kSmsQueuePending;
        smsQueueGrid(initial);
        *pc.inputHeader = initial;
    }
    if (pc.operation <= 1u) {
        SmsQueueHeader output = {};
        output.mode = 1u;
        output.capacity = pc.capacity;
        output.status = kSmsQueuePending;
        output.error = pc.inputHeader->error;
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
