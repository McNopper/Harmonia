// PERF4 feasibility probe — miss: sentinel payload (distance -1, id 0xFFFFFFFF).

struct RayPayload {
    float distance;
    uint primitiveId;
};

[shader("miss")]
void main(inout RayPayload payload) {
    payload.distance = -1.0;
    payload.primitiveId = 0xFFFFFFFFu;
}
