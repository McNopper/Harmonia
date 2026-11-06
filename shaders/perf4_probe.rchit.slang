// PERF4 feasibility probe — closest hit: record the hit distance + primitive id.

struct RayPayload {
    float distance;
    uint primitiveId;
};

[shader("closesthit")]
void main(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attributes) {
    payload.distance = RayTCurrent();
    payload.primitiveId = PrimitiveIndex();
}
