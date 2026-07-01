/// Shared GPU RNG / sampler utilities used by Harmonia consumers.
///
/// A0(a): the draw sequence is an **Owen-scrambled Sobol'** low-discrepancy sampler
/// (Burley 2020, "Practical Hash-based Owen Scrambling") rather than Wang-hash white
/// noise. Error converges faster for the same sample budget and distributes more evenly,
/// which the temporal denoiser reconstructs better. The estimator is unchanged/unbiased —
/// this only changes *which* stratified points are drawn, keyed by pixel + sample index +
/// dimension. `RngState` carries a stable per-(pixel,frame) scramble key, the Sobol sample
/// index (the accumulation frame / spp index), and a per-draw dimension counter.

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

/// Sampler state: `key` decorrelates per pixel/frame (stable within one path), `index` is the
/// Sobol' sample index (accumulation frame / spp), `dim` advances once per scalar draw.
struct RngState
{
    uint key;
    uint index;
    uint dim;
};

/// Hash-based Owen scramble of a 32-bit fixed-point value (Burley 2020, eq. via Laine-Karras
/// style mixing). Nested-uniform scrambling that preserves the sequence's stratification while
/// decorrelating it per seed.
uint owenScramble(uint x, uint seed)
{
    x = reversebits(x);
    x ^= x * 0x3d20adeau;
    x += seed;
    x *= (seed >> 16u) | 1u;
    x ^= x * 0x05526c56u;
    x ^= x * 0x53a22864u;
    return reversebits(x);
}

/// Sobol' 32-bit integer point for dimension 0 (van der Corput, base 2) or 1 (the classic
/// (0,2)-sequence second component). Higher dimensions are formed by padding: pairs of
/// dimensions reuse this 2D (0,2)-sequence and are decorrelated by the Owen scramble seed.
uint sobolInt(uint index, uint component)
{
    if (component == 0u)
    {
        return reversebits(index);
    }
    uint result = 0u;
    uint v = 1u << 31;
    while (index != 0u)
    {
        if ((index & 1u) != 0u)
        {
            result ^= v;
        }
        index >>= 1;
        v ^= v >> 1;
    }
    return result;
}

/// One Owen-scrambled Sobol' sample in [0,1) for a given sample index + dimension + key.
float sobolOwenSample(uint index, uint dim, uint key)
{
    uint pair = dim >> 1u;        // 2D pair this dimension belongs to
    uint component = dim & 1u;    // component (0/1) within the (0,2)-sequence pair
    uint pairKey = hashCombine(key, pair);
    // Owen-scramble the sample index (progressive decorrelation), then the value.
    uint scrambledIndex = owenScramble(index, hashCombine(pairKey, 0x68bc21ebu));
    uint x = sobolInt(scrambledIndex, component);
    x = owenScramble(x, hashCombine(pairKey, component + 1u));
    // 1 / 2^32, clamped strictly below 1 to keep [0,1) like the old mapping.
    return min(float(x) * 2.3283064365386963e-10, 0.99999994);
}

/// Build sampler state. `frameSampleIndex` is the Sobol' sample index; `bounceIndex` offsets the
/// dimension so a caller that re-seeds per bounce still draws decorrelated dimensions (callers
/// that seed once and thread `RngState` get one continuous low-discrepancy path).
RngState composeRngSeed(uint2 pixel, uint frameSampleIndex, uint bounceIndex, uint baseSeed)
{
    uint key = rngHash(baseSeed);
    key = hashCombine(key, pixel.x);
    key = hashCombine(key, pixel.y);

    RngState state;
    state.key = key;
    state.index = frameSampleIndex;
    state.dim = bounceIndex * 8u;
    return state;
}

/// Build sampler state directly from a pre-hashed key and a sample index (used where a scalar
/// per-pixel key is already available, e.g. the ray-generation stage).
RngState makeRngState(uint key, uint frameSampleIndex)
{
    RngState state;
    state.key = rngHash(key);
    state.index = frameSampleIndex;
    state.dim = 0u;
    return state;
}

float rngNextFloat(inout RngState state)
{
    float value = sobolOwenSample(state.index, state.dim, state.key);
    state.dim += 1u;
    return value;
}

float2 rngNextFloat2(inout RngState state)
{
    // Draw a stratified 2D pair (Sobol' dimensions form a (0,2)-sequence in each pair).
    float2 value = float2(sobolOwenSample(state.index, state.dim, state.key),
                          sobolOwenSample(state.index, state.dim + 1u, state.key));
    state.dim += 2u;
    return value;
}
