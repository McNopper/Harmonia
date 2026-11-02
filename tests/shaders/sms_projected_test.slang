import common;
import sms_solver;

[[vk::binding(0, 0)]] uniform RaytracingAccelerationStructure testTlas;
struct SolveInput { float4 receiver, light, seed; };
struct SolveOutput { float4 first, second; };
struct SolvePush { SolveInput* inputs; SolveOutput* outputs; uint count, pad; };
[[vk::push_constant]] SolvePush pc;

// Test adapter: actual hardware hits on finite BLAS triangles, with known planar
// differential frames. Primitive groups identify entry/exit/occluder surfaces.
struct SceneProjector : ISmsProjector {
    uint calls;
    bool corruptTopology;
    bool trace(float3 origin, float3 direction, uint plane, SmsManifoldVertex reference,
        out SmsManifoldVertex vertex) {
        vertex = reference;
        RayDesc ray;
        ray.Origin = origin;
        ray.Direction = direction;
        ray.TMin = kMediumEpsilon;
        ray.TMax = kInf;
        RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
        query.TraceRayInline(testTlas, RAY_FLAG_NONE, kInstanceMaskAll, ray);
        while (query.Proceed()) {}
        if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT ||
            query.CommittedPrimitiveIndex() / 2u != plane || query.CommittedInstanceID() != reference.objectKey.x)
            return false;
        vertex.position = origin + direction * query.CommittedRayT();
        vertex.geoNormal = float3(0, 0, plane == 0u ? 1.0 : -1.0);
        vertex.normal = vertex.geoNormal;
        vertex.dpdu = float3(1, 0, 0);
        vertex.dpdv = float3(0, 1, 0);
        vertex.dndu = vertex.dndv = 0.0.xxx;
        vertex.objectKey = uint2(query.CommittedInstanceID(), plane);
        return true;
    }
    [mutating]
    bool project(float3 receiver, float3 proposedFirst, SmsManifoldVertex reference[2],
        uint count, out SmsManifoldVertex projected[2]) {
        ++calls;
        projected[0] = reference[0]; projected[1] = reference[1];
        float3 edge = proposedFirst - receiver;
        if (!(dot(edge, edge) > 0.0)) return false;
        if (!trace(receiver, normalize(edge), 0u, reference[0], projected[0])) return false;
        for (uint i = 1u; i < count; ++i) {
            float3 previous = receiver;
            if (i > 1u) previous = projected[i - 2u].position;
            float3 wo = normalize(previous - projected[i - 1u].position);
            float3 n = projected[i - 1u].normal;
            if (dot(n, wo) < 0.0) n = -n;
            float3 wi; float weight;
            if (!smsDeltaInterface(wo, n, projected[i - 1u].event,
                1.0 / projected[i - 1u].etaTOverI, wi, weight)) return false;
            if (!trace(projected[i - 1u].position, wi, i, reference[i], projected[i])) return false;
        }
        // Fault-injection adapter: the shared solver must reject metadata changes
        // even if an adapter accidentally returns an otherwise valid projection.
        if (corruptTopology && calls > 1u) projected[0].objectKey.x += 1u;
        return true;
    }
};

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= pc.count) return;
    SolveInput input = pc.inputs[id.x];
    uint count = uint(input.receiver.w);
    SmsManifoldVertex reference[2];
    for (uint i = 0u; i < 2u; ++i) {
        reference[i].position = input.seed.xyz + float3(0, 0, -0.8 * float(i));
        reference[i].normal = reference[i].geoNormal = float3(0, 0, i == 0u ? 1.0 : -1.0);
        reference[i].dpdu = float3(1, 0, 0); reference[i].dpdv = float3(0, 1, 0);
        reference[i].dndu = reference[i].dndv = 0.0.xxx;
        reference[i].event = count == 1u ? kSmsReflection : kSmsTransmission;
        reference[i].etaTOverI = i == 0u ? input.seed.w : 1.0 / input.seed.w;
        reference[i].objectKey = uint2(7u, i);
    }
    SceneProjector projector;
    projector.calls = 0u;
    projector.corruptTopology = input.light.w == 1.0;
    SmsManifoldVertex vertices[2];
    vertices[0] = reference[0]; vertices[1] = reference[1];
    bool success = projector.project(input.receiver.xyz, input.seed.xyz, reference, count, vertices);
    if (success) success = solveSmsManifold(projector, input.receiver.xyz, input.light.xyz, vertices, count, 32u, 1.0e-5);
    SolveOutput output;
    output.first = float4(vertices[0].position, success ? 1.0 : 0.0);
    output.second = float4(vertices[1].position, float(projector.calls));
    pc.outputs[id.x] = output;
}
