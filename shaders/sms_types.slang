// Importable without renderer globals, for physical-address push-constant fields.
struct SmsWorkRecord { uint4 words[16]; }; // frozen float bits/integer state, 256B
static const uint kSmsAbsent = 0u;
static const uint kSmsPrepare = 1u;
static const uint kSmsInitial = 2u;
static const uint kSmsRepeat = 3u;
static const uint kSmsDone = 4u;
