// Specular manifold differential geometry, independent of renderer data layouts.
// Implicit-function theorem: dX/dY = -(dC/dX)^-1 (dC/dY).
// Zeltner et al. 2020, manifold geometric term; NO geometric-term clamps.
import math;

static const uint kSmsReflection = 0u;
static const uint kSmsTransmission = 1u;

struct SmsManifoldVertex {
    float3 position;
    float3 normal;       // unit shading normal
    float3 geoNormal;    // RAW outward-winding normal
    float3 dpdu, dpdv;   // surface parameter derivatives, not necessarily unit/orthogonal
    float3 dndu, dndv;   // derivatives of the unit shading normal in the SAME parameterization
    float etaTOverI;     // n_transmitted / n_incident (side-correct), transmission only
    uint event;
};

struct SmsConstraintBlocks {
    float2 value;
    float2x2 previous, current, next;
};

float3 smsDirectionDerivative(float3 w, float invDistance, float3 delta) {
    return (delta - w * dot(w, delta)) * invDistance;
}

float3 smsHalfDerivative(float3 h, float invLength, float3 delta) {
    return (delta - h * dot(h, delta)) * invLength;
}

bool smsConstraintBlocks(float3 previousPosition, SmsManifoldVertex v, float3 nextPosition,
    float3 previousDu, float3 previousDv, float3 nextDu, float3 nextDv,
    out SmsConstraintBlocks blocks) {
    blocks.value = 0.0.xx;
    blocks.previous = float2x2(0.0);
    blocks.current = float2x2(0.0);
    blocks.next = float2x2(0.0);
    float3 a = previousPosition - v.position;
    float3 b = nextPosition - v.position;
    float ra = length(a), rb = length(b);
    if (!(ra > 0.0 && rb > 0.0)) return false;
    float3 wi = a / ra, wo = b / rb;
    float eta = v.event == kSmsReflection ? 1.0 : v.etaTOverI;
    if (!(eta > 0.0) || !isfinite(eta)) return false;
    float3 hraw = wi + eta * wo;
    float hLength = length(hraw);
    if (!(hLength > 0.0) || !isfinite(hLength)) return false;
    float3 h = hraw / hLength;
    // Pullback of the normal constraint to the actual surface parameterization.
    float3 s = v.dpdu - v.normal * dot(v.dpdu, v.normal);
    float3 t = v.dpdv - v.normal * dot(v.dpdv, v.normal);
    blocks.value = float2(dot(h, s), dot(h, t));
    float3 previousAxes[2] = { previousDu, previousDv };
    float3 currentAxes[2] = { v.dpdu, v.dpdv };
    float3 normalAxes[2] = { v.dndu, v.dndv };
    float3 nextAxes[2] = { nextDu, nextDv };
    for (uint j = 0u; j < 2u; ++j) {
        float3 dhPrevious = smsHalfDerivative(h, 1.0 / hLength,
            smsDirectionDerivative(wi, 1.0 / ra, previousAxes[j]));
        float3 dhNext = smsHalfDerivative(h, 1.0 / hLength,
            eta * smsDirectionDerivative(wo, 1.0 / rb, nextAxes[j]));
        float3 dhCurrent = smsHalfDerivative(h, 1.0 / hLength,
            -smsDirectionDerivative(wi, 1.0 / ra, currentAxes[j])
            -eta * smsDirectionDerivative(wo, 1.0 / rb, currentAxes[j]));
        blocks.previous[0][j] = dot(s, dhPrevious);
        blocks.previous[1][j] = dot(t, dhPrevious);
        blocks.next[0][j] = dot(s, dhNext);
        blocks.next[1][j] = dot(t, dhNext);
        // Tangent-plane movement changes shading normals on curved/smooth geometry.
        // The omitted second position derivatives cancel at an admissible root.
        float3 dn = normalAxes[j];
        blocks.current[0][j] = dot(s, dhCurrent)
            - dot(v.dpdu, dn) * dot(h, v.normal) - dot(v.dpdu, v.normal) * dot(h, dn);
        blocks.current[1][j] = dot(t, dhCurrent)
            - dot(v.dpdv, dn) * dot(h, v.normal) - dot(v.dpdv, v.normal) * dot(h, dn);
    }
    return all(isfinite(blocks.value));
}

// Fixed 4x4 elimination with partial pivoting and two RHS columns. A one-vertex
// manifold uses an identity block for the unused rows. No inverse() intrinsic,
// shaderInt64, geometry clamping or silently substituted singular inverse.
bool smsSolveManifoldSystem(inout float4 rows[4], inout float2 rhs[4]) {
    for (uint col = 0u; col < 4u; ++col) {
        uint pivot = col;
        for (uint r = col + 1u; r < 4u; ++r)
            if (abs(rows[r][col]) > abs(rows[pivot][col])) pivot = r;
        float value = rows[pivot][col];
        if (value == 0.0 || !isfinite(value)) return false;
        if (pivot != col) {
            float4 savedRow = rows[col];
            float2 savedRhs = rhs[col];
            rows[col] = rows[pivot];
            rhs[col] = rhs[pivot];
            rows[pivot] = savedRow;
            rhs[pivot] = savedRhs;
        }
        rows[col] /= value;
        rhs[col] /= value;
        for (uint r = 0u; r < 4u; ++r) {
            if (r == col) continue;
            float scale = rows[r][col];
            rows[r] -= scale * rows[col];
            rhs[r] -= scale * rhs[col];
        }
    }
    for (uint r = 0u; r < 4u; ++r)
        if (!all(isfinite(rows[r])) || !all(isfinite(rhs[r]))) return false;
    return true;
}

bool smsAdmissibleEvent(float3 previous, SmsManifoldVertex v, float3 next) {
    float a = dot(previous - v.position, v.geoNormal);
    float b = dot(next - v.position, v.geoNormal);
    if (v.event == kSmsReflection) return a * b > 0.0;
    if (v.event == kSmsTransmission) return a * b < 0.0 && v.etaTOverI != 1.0;
    return false;
}

// Return d(omega at receiver)/d(area at light), for an ALREADY SOLVED manifold.
// lightDu/lightDv span one unit of emitter area; their handedness is irrelevant.
// False means invalid/singular geometry, NOT permission to clamp or drop an
// otherwise completed estimate. The caller owns an explicit failure policy.
bool smsGeometricTerm(float3 receiver, SmsManifoldVertex vertices[2], uint count,
    float3 light, float3 lightDu, float3 lightDv, out float geometric) {
    geometric = 0.0;
    if (count < 1u || count > 2u) return false;
    float4 rows[4];
    float2 rhs[4];
    for (uint r = 0u; r < 4u; ++r) { rows[r] = 0.0.xxxx; rhs[r] = 0.0.xx; }
    if (count == 1u) { rows[2].z = 1.0; rows[3].w = 1.0; }
    for (uint i = 0u; i < count; ++i) {
        float3 previous = receiver;
        float3 next = light;
        float3 previousDu = 0.0.xxx, previousDv = 0.0.xxx;
        float3 nextDu = lightDu, nextDv = lightDv;
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
        if (!smsAdmissibleEvent(previous, vertices[i], next)) return false;
        SmsConstraintBlocks blocks;
        if (!smsConstraintBlocks(previous, vertices[i], next, previousDu, previousDv, nextDu, nextDv, blocks))
            return false;
        for (uint r = 0u; r < 2u; ++r) {
            for (uint c = 0u; c < 2u; ++c) {
                rows[2u * i + r][2u * i + c] = blocks.current[r][c];
                if (i > 0u) rows[2u * i + r][2u * i - 2u + c] = blocks.previous[r][c];
                if (i + 1u < count) rows[2u * i + r][2u * i + 2u + c] = blocks.next[r][c];
                else rhs[2u * i + r][c] = -blocks.next[r][c];
            }
        }
    }
    if (!smsSolveManifoldSystem(rows, rhs)) return false;
    float determinant = rhs[0].x * rhs[1].y - rhs[0].y * rhs[1].x;
    float3 firstEdge = receiver - vertices[0].position;
    float distance2 = dot(firstEdge, firstEdge);
    if (!(distance2 > 0.0)) return false;
    float solidAngle = abs(dot(firstEdge / sqrt(distance2), cross(vertices[0].dpdu, vertices[0].dpdv))) / distance2;
    float result = abs(determinant) * solidAngle;
    if (!isfinite(result) || result < 0.0) return false;
    geometric = result;
    return true;
}

// Pure interface factors, using the existing shared Fresnel function. Directions
// point away from the vertex; normal faces incoming wo. Transmission rejection
// on TIR is explicit: it must NOT silently change the requested chain topology.
bool smsDeltaInterface(float3 wo, float3 normal, uint event, float etaIOverT,
    out float3 wi, out float radianceWeight) {
    wi = 0.0.xxx;
    radianceWeight = 0.0;
    if (!(etaIOverT > 0.0) || !isfinite(etaIOverT)) return false;
    float fresnel = fresnelDielectric(dot(wo, normal), 1.0 / etaIOverT);
    if (event == kSmsReflection) {
        wi = reflect(-wo, normal);
        radianceWeight = fresnel;
    } else if (event == kSmsTransmission) {
        wi = refract(-wo, normal, etaIOverT);
        if (dot(wi, wi) == 0.0) return false;
        radianceWeight = (1.0 - fresnel) * etaIOverT * etaIOverT;
    } else return false;
    return all(isfinite(wi)) && isfinite(radianceWeight);
}
