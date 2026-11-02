// Bounded manifold solve for a fixed specular chain; every accepted proposal is
// reprojected by the renderer/scene adapter. This is not an unbounded trial loop.
__exported import sms_geometry;

interface ISmsProjector {
    [mutating]
    bool project(float3 receiver, float3 proposedFirst, SmsManifoldVertex reference[2],
        uint count, out SmsManifoldVertex projected[2]);
};

bool smsManifoldResidual(float3 receiver, SmsManifoldVertex vertices[2], uint count,
    float3 light, out float residual) {
    residual = 0.0;
    if (count < 1u || count > 2u) return false;
    for (uint i = 0u; i < count; ++i) {
        float3 previous = receiver, next = light;
        if (i > 0u) previous = vertices[i - 1u].position;
        if (i + 1u < count) next = vertices[i + 1u].position;
        float3 a = previous - vertices[i].position;
        float3 b = next - vertices[i].position;
        float ra = length(a), rb = length(b);
        if (!(ra > 0.0 && rb > 0.0)) return false;
        float eta = vertices[i].event == kSmsReflection ? 1.0 : vertices[i].etaTOverI;
        float3 halfVector = a / ra + eta * b / rb;
        float hLength = length(halfVector);
        if (!(hLength > 0.0) || !isfinite(hLength)) return false;
        float error = length(cross(halfVector / hLength, vertices[i].normal));
        if (!isfinite(error)) return false;
        residual = max(residual, error);
    }
    return true;
}

bool smsValidSolvedEvents(float3 receiver, SmsManifoldVertex vertices[2], uint count, float3 light) {
    for (uint i = 0u; i < count; ++i) {
        float3 previous = receiver, next = light;
        if (i > 0u) previous = vertices[i - 1u].position;
        if (i + 1u < count) next = vertices[i + 1u].position;
        if (!smsAdmissibleEvent(previous, vertices[i], next)) return false;
    }
    return true;
}

bool solveSmsManifold<P : ISmsProjector>(inout P projector, float3 receiver, float3 light,
    inout SmsManifoldVertex vertices[2], uint count, uint maxIterations, float tolerance) {
    if (count < 1u || count > 2u || maxIterations == 0u ||
        !(tolerance > 0.0) || !isfinite(tolerance)) return false;
    for (uint iteration = 0u; iteration < maxIterations; ++iteration) {
        float residual;
        if (!smsManifoldResidual(receiver, vertices, count, light, residual)) return false;
        if (residual <= tolerance) return smsValidSolvedEvents(receiver, vertices, count, light);
        float4 rows[4];
        float2 rhs[4];
        for (uint r = 0u; r < 4u; ++r) { rows[r] = 0.0.xxxx; rhs[r] = 0.0.xx; }
        if (count == 1u) { rows[2].z = 1.0; rows[3].w = 1.0; }
        for (uint i = 0u; i < count; ++i) {
            float3 previous = receiver, next = light;
            float3 previousDu = 0.0.xxx, previousDv = 0.0.xxx;
            float3 nextDu = 0.0.xxx, nextDv = 0.0.xxx;
            if (i > 0u) {
                previous = vertices[i - 1u].position;
                previousDu = vertices[i - 1u].dpdu;
                previousDv = vertices[i - 1u].dpdv;
            }
            if (i + 1u < count) {
                next = vertices[i + 1u].position;
                nextDu = vertices[i + 1u].dpdu;
                nextDv = vertices[i + 1u].dpdv;
            }
            SmsConstraintBlocks blocks;
            if (!smsConstraintBlocks(previous, vertices[i], next, previousDu, previousDv, nextDu, nextDv, blocks))
                return false;
            for (uint r = 0u; r < 2u; ++r) {
                rhs[2u * i + r].x = -blocks.value[r];
                for (uint c = 0u; c < 2u; ++c) {
                    rows[2u * i + r][2u * i + c] = blocks.current[r][c];
                    if (i > 0u) rows[2u * i + r][2u * i - 2u + c] = blocks.previous[r][c];
                    if (i + 1u < count) rows[2u * i + r][2u * i + 2u + c] = blocks.next[r][c];
                }
            }
        }
        if (!smsSolveManifoldSystem(rows, rhs)) return false;
        float3 firstStep = vertices[0].dpdu * rhs[0].x + vertices[0].dpdv * rhs[1].x;
        bool accepted = false;
        float beta = 1.0;
        for (uint attempt = 0u; attempt < 8u; ++attempt) {
            SmsManifoldVertex proposed[2];
            float3 proposedFirst = vertices[0].position + beta * firstStep;
            if (projector.project(receiver, proposedFirst, vertices, count, proposed)) {
                bool fixedTopology = true;
                for (uint i = 0u; i < count; ++i) {
                    fixedTopology = fixedTopology && all(proposed[i].objectKey == vertices[i].objectKey) &&
                        proposed[i].event == vertices[i].event && proposed[i].etaTOverI == vertices[i].etaTOverI;
                }
                float nextResidual;
                if (fixedTopology && smsManifoldResidual(receiver, proposed, count, light, nextResidual) &&
                    (nextResidual < residual || nextResidual <= tolerance)) {
                    for (uint i = 0u; i < count; ++i) vertices[i] = proposed[i];
                    accepted = true;
                    break;
                }
            }
            beta *= 0.5;
        }
        if (!accepted) return false;
    }
    float finalResidual;
    return smsManifoldResidual(receiver, vertices, count, light, finalResidual) &&
        finalResidual <= tolerance && smsValidSolvedEvents(receiver, vertices, count, light);
}

// Numerical root equivalence for repeat trials. Keep the topology and event/IOR
// side fixed; do not identify roots from a coarse dot-product cancellation test.
bool sameSmsRoot(float3 receiver, SmsManifoldVertex a[2], uint countA,
    SmsManifoldVertex b[2], uint countB, float tolerance) {
    if (countA < 1u || countA > 2u || countA != countB || !(tolerance > 0.0) || !isfinite(tolerance))
        return false;
    float3 previousA = receiver, previousB = receiver;
    for (uint i = 0u; i < countA; ++i) {
        if (any(a[i].objectKey != b[i].objectKey) || a[i].event != b[i].event ||
            a[i].etaTOverI != b[i].etaTOverI) return false;
        float3 da = a[i].position - previousA, db = b[i].position - previousB;
        float la = length(da), lb = length(db);
        if (!(la > 0.0 && lb > 0.0) || !isfinite(la) || !isfinite(lb)) return false;
        float difference = length(da / la - db / lb);
        if (!isfinite(difference) || difference > tolerance) return false;
        previousA = a[i].position;
        previousB = b[i].position;
    }
    return true;
}
