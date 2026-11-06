// PERF4 feasibility probe — raygen: reads one ray record (32 B, 8 floats — no
// float3 packing ambiguity), traces it, writes the payload record.
// Validity contract: identical payloads per ray across orderings (sorted arms
// trace the same rays in a different order; the test reorders outputs back and
// compares exactly).

struct RayRecord {
    float ox;
    float oy;
    float oz;
    float tmax;
    float dx;
    float dy;
    float dz;
    float pad;
};

struct RayPayload {
    float distance;
    uint primitiveId;
};

[[vk::binding(0, 0)]]
StructuredBuffer<RayRecord> g_rays;

[[vk::binding(1, 0)]]
uniform RaytracingAccelerationStructure g_scene;

[[vk::binding(2, 0)]]
RWStructuredBuffer<RayPayload> g_payloads;

[shader("raygen")]
void main() {
    const uint index = DispatchRaysIndex().x;
    RayRecord record = g_rays[index];

    RayDesc ray;
    ray.Origin = float3(record.ox, record.oy, record.oz);
    ray.Direction = normalize(float3(record.dx, record.dy, record.dz));
    ray.TMin = 0.001;
    ray.TMax = record.tmax;

    RayPayload payload;
    payload.distance = -1.0;
    payload.primitiveId = 0xFFFFFFFFu;
    TraceRay(g_scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);
    g_payloads[index] = payload;
}
