import sms_geometry;

struct GeometryInput {
    float4 receiver; // w = vertex count (geometry) or event (interface)
    float4 light;    // w = 0 geometry, 1 interface
    float4 vertexData[14];
    float4 lightDu, lightDv;
};
struct GeometryOutput { float4 value, status; };
struct GeometryPush { GeometryInput* inputs; GeometryOutput* outputs; uint count, pad; };
[[vk::push_constant]] GeometryPush pc;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= pc.count) return;
    GeometryInput input = pc.inputs[id.x];
    GeometryOutput output;
    output.value = 0.0.xxxx;
    output.status = 0.0.xxxx;
    bool success;
    if (input.light.w == 1.0) {
        float3 wi;
        float weight;
        success = smsDeltaInterface(input.receiver.xyz, input.vertexData[0].xyz,
            uint(input.receiver.w), input.vertexData[0].w, wi, weight);
        output.value = float4(wi, weight);
    } else {
        SmsManifoldVertex vertices[2];
        for (uint i = 0u; i < 2u; ++i) {
            uint base = i * 7u;
            vertices[i].position = input.vertexData[base].xyz;
            vertices[i].etaTOverI = input.vertexData[base].w;
            vertices[i].normal = input.vertexData[base + 1u].xyz;
            vertices[i].event = uint(input.vertexData[base + 1u].w);
            vertices[i].objectKey = uint2(0u, 0u);
            vertices[i].geoNormal = input.vertexData[base + 2u].xyz;
            vertices[i].dpdu = input.vertexData[base + 3u].xyz;
            vertices[i].dpdv = input.vertexData[base + 4u].xyz;
            vertices[i].dndu = input.vertexData[base + 5u].xyz;
            vertices[i].dndv = input.vertexData[base + 6u].xyz;
        }
        float geometric;
        success = smsGeometricTerm(input.receiver.xyz, vertices, uint(input.receiver.w),
            input.light.xyz, input.lightDu.xyz, input.lightDv.xyz, geometric);
        output.value.x = geometric;
    }
    output.status.x = success ? 1.0 : 0.0;
    pc.outputs[id.x] = output;
}
