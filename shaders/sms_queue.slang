// Shared GPU queue ABI. No renderer globals or push-constant block in this module.
static const uint kSmsQueuePending = 0u;
static const uint kSmsQueueReady = 1u;
static const uint kSmsQueueFailed = 2u;
static const uint kSmsQueueOverflow = 1u;
static const uint kSmsQueueInvalidWork = 2u;
static const uint kSmsQueueNumericError = 4u;

struct SmsWorkRecord { uint4 words[16]; }; // frozen float bits/integer state, 256B

struct SmsQueueHeader {
    uint dispatchX, dispatchY, dispatchZ, count;
    uint mode, status, error, capacity;
    uint4 reserved[2];
}; // 64B; first 12 bytes are VkDispatchIndirectCommand

struct SmsQueuePush {
    SmsWorkRecord* work;
    uint* inputIndices;
    uint* outputIndices;
    SmsQueueHeader* inputHeader;
    SmsQueueHeader* outputHeader;
    uint capacity, operation, trialBudget, pad;
}; // 56B, 8-byte alignment

void smsQueueError(SmsQueuePush pc, uint error) {
    uint ignored;
    InterlockedOr(pc.outputHeader->error, error, ignored);
}

// A producer may append its slot at most once per invocation. In particular,
// pending reciprocal estimates are appended, never converted into zero samples.
void smsQueueAppend(SmsQueuePush pc, uint slot) {
    if (slot >= pc.capacity) {
        smsQueueError(pc, kSmsQueueInvalidWork);
        return;
    }
    uint index;
    InterlockedAdd(pc.outputHeader->count, 1u, index);
    if (index >= pc.capacity) {
        smsQueueError(pc, kSmsQueueOverflow);
        return;
    }
    pc.outputIndices[index] = slot;
}

// Guard group padding BEFORE multiplying by 64: the final padded 2D grid may
// otherwise wrap uint for very large slot counts. Counts/dispatches stay on GPU.
bool smsQueueIndex(SmsQueueHeader header, uint3 group, uint lane, out uint index) {
    index = 0u;
    uint count = header.count;
    if (header.error != 0u || count == 0u || count > header.capacity) return false;
    uint groupIndex = group.y * header.dispatchX + group.x;
    if (groupIndex > (count - 1u) / 64u) return false;
    index = groupIndex * 64u + lane;
    if (index >= count) return false;
    return true;
}

bool smsQueueSlot(SmsQueuePush pc, uint3 group, uint lane, out uint slot) {
    slot = 0u;
    if (pc.inputHeader->count > pc.capacity) {
        smsQueueError(pc, kSmsQueueInvalidWork);
        return false;
    }
    uint index;
    if (!smsQueueIndex(*pc.inputHeader, group, lane, index)) return false;
    slot = pc.inputHeader->mode == 0u ? index : pc.inputIndices[index];
    if (slot >= pc.capacity) {
        smsQueueError(pc, kSmsQueueInvalidWork);
        return false;
    }
    return true;
}

void smsQueueGrid(inout SmsQueueHeader header) {
    uint groups = header.count / 64u + (header.count % 64u != 0u ? 1u : 0u);
    header.dispatchX = min(groups, 65535u);
    header.dispatchY = groups == 0u ? 1u : (groups / header.dispatchX +
        (groups % header.dispatchX != 0u ? 1u : 0u));
    header.dispatchZ = 1u;
}
