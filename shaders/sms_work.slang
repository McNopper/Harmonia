// Frozen receiver/light/root ABI used by RT preparation, the ONE compute trial
// site, and ready-only resolve. All floating values are persisted as raw bits.
__exported import sms_queue;
import common;
import sms_transport;

// words: 0 receiver P/material, 1 raw N/phase, 2 raw Ng/uv.x,
// 3 raw tangent/uv.y, 4 wo/pixel, 5 light P/area pdf, 6 light N/Le.r,
// 7 Le.gb/Philox key, 8/9 root P/instance (9.xyz holds prefix RGB until root storage), 10 root materials/count/event,
// 11 root's unweighted RGB / P(C), 12 reciprocal low/high/status / tile resolution,
// 13 root eta0/eta1/emitterCount (then light ID)/remaining receiver-inclusive depth,
// 14 exposure/spp/frame/seed (then conditional origin),
// 15 sample/reserved (then conditional extent)/width/height. No renderer descriptors here.
uint smsPhase(SmsWorkRecord work) { return work.words[1].w; }
void smsSetPhase(inout SmsWorkRecord work, uint phase) { work.words[1].w = phase; }
float3 smsReceiverPosition(SmsWorkRecord work) { return asfloat(work.words[0].xyz); }
float3 smsLightPosition(SmsWorkRecord work) { return asfloat(work.words[5].xyz); }

void smsStoreRoot(inout SmsWorkRecord work, SmsManifoldVertex root[2], uint count) {
    work.words[8] = uint4(asuint(root[0].position), root[0].objectKey.x);
    work.words[9] = uint4(asuint(root[1].position), root[1].objectKey.x);
    work.words[10] = uint4(root[0].objectKey.y, root[1].objectKey.y, count, root[0].event);
    work.words[13].xy = uint2(asuint(root[0].etaTOverI), asuint(root[1].etaTOverI));
}

void smsLoadRoot(SmsWorkRecord work, out SmsManifoldVertex root[2], out uint count) {
    root[0] = root[1] = (SmsManifoldVertex)0;
    root[0].position = asfloat(work.words[8].xyz);
    root[1].position = asfloat(work.words[9].xyz);
    root[0].objectKey = uint2(work.words[8].w, work.words[10].x);
    root[1].objectKey = uint2(work.words[9].w, work.words[10].y);
    root[0].event = work.words[10].w;
    root[1].event = kSmsTransmission;
    root[0].etaTOverI = asfloat(work.words[13].x);
    root[1].etaTOverI = asfloat(work.words[13].y);
    count = work.words[10].z;
    // sameSmsRoot uses only positions, keys, events and eta, not differentials.
}
