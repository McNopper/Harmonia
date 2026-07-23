import common;
__exported import rng;

float sqr(float x)
{
    return x * x;
}

/// Largest component of a float3.
float maxComp(float3 v)
{
    return max(v.x, max(v.y, v.z));
}

float safeSqrt(float x)
{
    return sqrt(max(x, 0.0));
}

float2 sampleDisk(float2 xi)
{
    float2 u = 2.0 * xi - 1.0;
    if (u.x == 0.0 && u.y == 0.0) {
        return 0.0.xx;
    }

    float r;
    float phi;
    if (abs(u.x) > abs(u.y)) {
        r = u.x;
        phi = (kPi * 0.25) * (u.y / u.x);
    } else {
        r = u.y;
        phi = (kPi * 0.5) - (kPi * 0.25) * (u.x / u.y);
    }

    return r * float2(cos(phi), sin(phi));
}

float3 sampleCosineHemisphere(float2 xi)
{
    float2 d = sampleDisk(xi);
    float z = safeSqrt(1.0 - dot(d, d));
    return float3(d.x, d.y, z);
}

float3 sampleGGX_VNDF(float3 Ve, float alphaX, float alphaY, float2 xi)
{
    float3 Vh = normalize(float3(alphaX * Ve.x, alphaY * Ve.y, Ve.z));

    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    float3 T1 = lensq > 0.0 ? float3(-Vh.y, Vh.x, 0.0) * rsqrt(lensq) : float3(1.0, 0.0, 0.0);
    float3 T2 = cross(Vh, T1);

    float r = sqrt(xi.x);
    float phi = 2.0 * kPi * xi.y;
    float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    float s = 0.5 * (1.0 + Vh.z);
    t2 = lerp(safeSqrt(max(0.0, 1.0 - t1 * t1)), t2, s);

    float3 Nh = t1 * T1 + t2 * T2 + safeSqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;
    float3 Ne = normalize(float3(alphaX * Nh.x, alphaY * Nh.y, max(0.0, Nh.z)));
    return Ne;
}

float GGX_D(float3 m, float alphaX, float alphaY)
{
    if (m.z <= 0.0) {
        return 0.0;
    }

    float sx = m.x / alphaX;
    float sy = m.y / alphaY;
    float d = sx * sx + sy * sy + m.z * m.z;
    return 1.0 / (kPi * alphaX * alphaY * d * d);
}

float GGX_Lambda(float3 v, float alphaX, float alphaY)
{
    float cosTheta = abs(v.z);
    if (cosTheta <= 0.0) {
        return 1.0e10;
    }

    float sinTheta2 = max(0.0, 1.0 - cosTheta * cosTheta);
    if (sinTheta2 <= 0.0) {
        return 0.0;
    }

    float invSinTheta = rsqrt(sinTheta2);
    float cosPhi = v.x * invSinTheta;
    float sinPhi = v.y * invSinTheta;
    float alpha2 = sqr(alphaX * cosPhi) + sqr(alphaY * sinPhi);
    float tanTheta2 = sinTheta2 / (cosTheta * cosTheta);
    return 0.5 * (sqrt(1.0 + alpha2 * tanTheta2) - 1.0);
}

float GGX_G1(float3 v, float alphaX, float alphaY)
{
    return 1.0 / (1.0 + GGX_Lambda(v, alphaX, alphaY));
}

float GGX_G2(float3 l, float3 v, float alphaX, float alphaY)
{
    return 1.0 / (1.0 + GGX_Lambda(l, alphaX, alphaY) + GGX_Lambda(v, alphaX, alphaY));
}

// VNDF PDF: probability density for the half-vector h given view direction v
// Used for importance sampling. Reference: Walter et al. 2007, Heitz 2018.
float GGX_VNDF_PDF(float3 h, float3 v, float alphaX, float alphaY)
{
    float D = GGX_D(h, alphaX, alphaY);
    float G1_v = GGX_G1(v, alphaX, alphaY);
    float dotVH = max(0.0, dot(v, h));
    if (dotVH <= 0.0) return 0.0;
    
    // p(h | v) = G1(v) * max(0, v · h) * D(h) / v.z
    // Simplified from the visibility-weighted formula
    return G1_v * dotVH * D / max(abs(v.z), 1.0e-5);
}

float3 fresnelSchlick(float3 F0, float cosTheta)
{
    float x = pow(1.0 - saturate(cosTheta), 5.0);
    return F0 + (1.0 - F0) * x;
}

float fresnelDielectric(float cosI, float eta)
{
    cosI = clamp(cosI, -1.0, 1.0);
    float ei = 1.0;
    float et = eta;
    if (cosI <= 0.0) {
        cosI = abs(cosI);
        ei = eta;
        et = 1.0;
    }

    float sinT2 = sqr(ei / et) * max(0.0, 1.0 - cosI * cosI);
    if (sinT2 >= 1.0) {
        return 1.0;
    }

    float cosT = safeSqrt(1.0 - sinT2);
    float rs = (ei * cosI - et * cosT) / (ei * cosI + et * cosT);
    float rp = (et * cosI - ei * cosT) / (et * cosI + ei * cosT);
    return 0.5 * (rs * rs + rp * rp);
}

float3x3 buildTBN(float3 N)
{
    float sign = N.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (sign + N.z);
    float b = N.x * N.y * a;
    float3 T = float3(1.0 + sign * N.x * N.x * a, sign * b, -sign * N.x);
    float3 B = float3(b, sign + N.y * N.y * a, -N.y);
    return float3x3(normalize(T), normalize(B), normalize(N));
}

/// Build an orthonormal frame whose Z axis points in direction `z`.
/// Row 0 = tangent (X), Row 1 = bitangent (Y), Row 2 = z (Z).
/// Used for sampling cones / disks aligned to an arbitrary axis.
float3x3 buildFrameFromZ(float3 z)
{
    // Frisvad / Duff numerical method: stable for all z directions.
    float sign = z.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (sign + z.z);
    float b = z.x * z.y * a;
    float3 t = float3(1.0 + sign * z.x * z.x * a, sign * b, -sign * z.x);
    float3 bv = float3(b, sign + z.y * z.y * a, -z.y);
    return float3x3(normalize(t), normalize(bv), z);
}

uint wangHash(uint seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed *= 9u;
    seed = seed ^ (seed >> 4u);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    return seed;
}

/// A0(a): `randFloat`/`randFloat2` now draw from the shared Owen-scrambled Sobol' sampler
/// (see rng.slang) instead of Wang-hash white noise. Same call sites, unbiased estimator,
/// lower-discrepancy (faster-converging, denoiser-friendlier) point sets.
float randFloat(inout RngState seed)
{
    return rngNextFloat(seed);
}

float2 randFloat2(inout RngState seed)
{
    return rngNextFloat2(seed);
}
