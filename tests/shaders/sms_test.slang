// Test entry point calls the production Slang functions, not a CPU translation.
import math;

struct SmsTestInput {
    float4 receiver;
    float4 light;
    float4 seed;
    float4 normal;
    float4 tangent;
    float4 bitangent;
};

struct SmsTestOutput {
    float4 root;       // xyz = result, w = solver success
    float4 constraint;
};

struct SmsTestPush {
    SmsTestInput* inputs;
    SmsTestOutput* outputs;
    uint count;
    uint pad;
};
[[vk::push_constant]] SmsTestPush pc;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= pc.count) return;
    SmsTestInput input = pc.inputs[id.x];
    float3 root = input.seed.xyz;
    bool success = manifoldWalk2Bounce(input.receiver.xyz, input.light.xyz, root,
        input.normal.xyz, 1.0, input.tangent.xyz, input.bitangent.xyz, 32, 1.0e-5);
    SmsTestOutput output;
    output.root = float4(root, success ? 1.0 : 0.0);
    float2 c = specularConstraint(normalize(input.receiver.xyz - root),
        normalize(input.light.xyz - root), input.normal.xyz, 1.0);
    output.constraint = float4(c, 0.0, 0.0);
    pc.outputs[id.x] = output;
}
