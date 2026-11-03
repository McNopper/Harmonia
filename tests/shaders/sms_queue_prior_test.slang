import sms_queue;
[[vk::push_constant]] SmsQueuePush pc;
[shader("compute")]
[numthreads(128,1,1)]
void main(uint3 group : SV_GroupID,uint lane : SV_GroupIndex) {
    if (any(group!=uint3(0,0,0)) || lane!=0u) return;
    uint ignored;
    InterlockedAdd(pc.userData[0],1u,ignored);
    pc.userData[1]=pc.inputHeader->mode;
    pc.userData[2]=pc.work[0].words[1].x;
    pc.userData[3]=pc.work[0].words[0].x;
}
