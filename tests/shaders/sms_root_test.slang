import sms_solver;
struct RootInput { float4 a0, a1, b0, b1; uint4 modes; };
struct RootPush { RootInput* inputs; uint4* outputs; uint count, pad; };
[[vk::push_constant]] RootPush pc;

SmsManifoldVertex rootVertex(float3 position, uint i) {
    SmsManifoldVertex v;
    v.position = position;
    v.normal = v.geoNormal = float3(0, 0, 1);
    v.dpdu = float3(1, 0, 0); v.dpdv = float3(0, 1, 0);
    v.dndu = v.dndv = 0.0.xxx;
    v.etaTOverI = 1.0;
    v.event = kSmsReflection;
    v.objectKey = uint2(7u, i);
    return v;
}
[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= pc.count) return;
    RootInput input = pc.inputs[id.x];
    SmsManifoldVertex a[2], b[2];
    a[0] = rootVertex(input.a0.xyz, 0u); a[1] = rootVertex(input.a1.xyz, 1u);
    b[0] = rootVertex(input.b0.xyz, 0u); b[1] = rootVertex(input.b1.xyz, 1u);
    if ((input.modes.z & 1u) != 0u) b[0].objectKey.x += 1u;
    if ((input.modes.z & 2u) != 0u) b[0].event = kSmsTransmission;
    if ((input.modes.z & 4u) != 0u) b[0].etaTOverI = 1.5;
    if ((input.modes.z & 8u) != 0u) b[1].objectKey.y += 1u;
    bool same = sameSmsRoot(float3(0, 0, 1), a, input.modes.x, b, input.modes.y, input.a0.w);
    pc.outputs[id.x] = uint4(same ? 1u : 0u, 0, 0, 0);
}
