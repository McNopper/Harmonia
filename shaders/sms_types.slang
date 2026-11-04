// Importable without renderer globals, for physical-address push-constant fields.
struct SmsWorkRecord { uint4 words[16]; }; // frozen float bits/integer state, 256B
static const uint kSmsAbsent = 0u;
static const uint kSmsPrepare = 1u;
static const uint kSmsInitial = 2u;
static const uint kSmsRepeat = 3u;
static const uint kSmsDone = 4u;

// Hong et al. 2025, section 5: spatiotemporal reservoir. The frozen candidate
// record keeps receiver/light/root state reconstructable by the shift walks;
// `weight` is the raw GRIS weight sum (consumers finalize by the selected
// target luminance) and `mass` is the M confidence in float bits. mass bits
// of zero mark an invalid reservoir; the trailing words pad the stride to 128B.
struct SmsReservoir {
    SmsWorkRecord record; // words[9] holds root storage here, never prefix RGB
    float4 prefix;        // receiver prefix RGB and the frozen radiometric proxy
    float3 estimate;      // selected candidate value in the owning pixel's domain
    float weight;         // raw GRIS weight sum in float bits (>= 0)
    float mass;           // M confidence in float bits (0 = invalid)
    uint count;           // selected candidate root count
    uint reserved0;
    uint reserved1;
    uint reserved2;
}; // 320B stride (record 256 + 16-byte-aligned tail; scalar tail avoids
   // PhysicalStorageBuffer vector straddles)
