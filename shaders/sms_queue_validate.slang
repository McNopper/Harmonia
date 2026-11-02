// Queue exhaustion alone is not successful completion: no live record may have
// been dropped. Runs before the waited host status observation, not in resolve
// alongside accumulation (which would detect an error too late).
import sms_queue;
[[vk::push_constant]] SmsQueuePush pc;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex) {
    uint slot;
    smsQueueValidationSlot(pc, group, lane, slot);
}
