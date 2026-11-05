// Importable without renderer globals, for physical-address push-constant fields.
struct SmsWorkRecord { uint4 words[16]; }; // frozen float bits/integer state, 256B
static const uint kSmsAbsent = 0u;
static const uint kSmsPrepare = 1u;
static const uint kSmsInitial = 2u;
static const uint kSmsRepeat = 3u;
static const uint kSmsDone = 4u;

// Hong et al. 2025, section 5: spatiotemporal reservoir. The frozen candidate
// record keeps receiver/light/root state reconstructable by the shift walks.
// `weight`/`mass` hold FLOAT BITS in uint storage: writing asuint(x) into a
// float field would convert the bit integer to a float VALUE (the bring-up
// defect: sane weights ~2e3 appeared as ~1.1e9 and compounded to overflow).
struct SmsReservoir {
    SmsWorkRecord record; // words[9] holds root storage here, never prefix RGB
    float4 prefix;        // receiver prefix RGB and the frozen radiometric proxy
    float3 estimate;      // selected candidate value in the owning pixel's domain
    uint weight;          // raw GRIS weight sum, float bits (asuint/asfloat)
    uint mass;            // M confidence, float bits; asuint(0.0) = invalid
    uint count;           // selected candidate root count
    uint reserved0;
    uint reserved1;
    uint reserved2;
}; // 320B stride (record 256 + 16-byte-aligned tail; scalar tail avoids
   // PhysicalStorageBuffer vector straddles)
