import aces;

struct AcesPush { float4* outputs; };
[[vk::push_constant]] AcesPush pc;

float3 fullFromAp0(float3 ap0) {
    float3 linear = acesOdtToLinearRec709(acesRrt(ap0));
    return float3(acesSrgbEncode(linear.x), acesSrgbEncode(linear.y), acesSrgbEncode(linear.z));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint mode = id.x;
    if (mode >= 5u) return;
    if (mode == 0u) {
        pc.outputs[mode] = float4(mul(kAcesAp1ToAp0, float3(.5, .4, .3)), 1);
    } else if (mode == 1u) {
        pc.outputs[mode] = float4(fullFromAp0(float3(.5, .4, .3)), 1);
    } else if (mode == 2u) {
        pc.outputs[mode] = float4(fullFromAp0(float3(.18, .18, .18)), 1);
    } else if (mode == 3u) {
        pc.outputs[mode] = float4(fullFromAp0(float3(1, 1, 1)), 1);
    } else {
        pc.outputs[mode] = float4(acesOdtToXyzD65(acesRrt(float3(.5, .4, .3))), 1);
    }
}
