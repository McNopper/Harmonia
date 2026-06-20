/// Shared GPU RNG utilities used by Harmonia consumers.
/// Seed composition decorrelates by pixel, frame/sample index and bounce index.

uint rngHash(uint seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed *= 9u;
    seed = seed ^ (seed >> 4u);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    return seed;
}

uint hashCombine(uint seed, uint value)
{
    return rngHash(seed ^ (value + 0x9e3779b9u + (seed << 6u) + (seed >> 2u)));
}

uint composeRngSeed(uint2 pixel, uint frameSampleIndex, uint bounceIndex, uint baseSeed)
{
    uint seed = rngHash(baseSeed);
    seed = hashCombine(seed, pixel.x);
    seed = hashCombine(seed, pixel.y);
    seed = hashCombine(seed, frameSampleIndex);
    seed = hashCombine(seed, bounceIndex);
    return seed;
}

float rngNextFloat(inout uint seed)
{
    seed = rngHash(seed);
    return (seed & 0x00ffffffu) / 16777216.0; // 2^24 — maps 24-bit mantissa to [0, 1)
}

float2 rngNextFloat2(inout uint seed)
{
    return float2(rngNextFloat(seed), rngNextFloat(seed));
}
