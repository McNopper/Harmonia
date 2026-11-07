#include "harmonia/core/PipelineBinaryCache.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

#include "harmonia/core/Logger.hpp"

namespace harmonia::core {
namespace {

// Entry-file container: [u32 magic][u32 version][u32 blobCount] then per blob
// [u32 keySize][key bytes][u64 dataSize][data bytes]. Named after the DRIVER's
// pipeline key (hex) so lookups need no index file. A torn/partial write fails
// parsing, gets deleted, and re-captures — the format self-heals by design.
constexpr std::uint32_t kEntryMagic = 0x43425048U; // 'HPBC' little-endian
constexpr std::uint32_t kEntryVersion = 1U;
constexpr const char* kEntryExtension = ".pbinc";
constexpr const char* kGlobalKeyFileName = "globalkey";

[[nodiscard]] VkPipelineBinaryKeyKHR makeKey() {
    VkPipelineBinaryKeyKHR key{};
    key.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR;
    key.pNext = nullptr;
    key.keySize = 0;
    return key;
}

[[nodiscard]] bool usableKey(const VkPipelineBinaryKeyKHR& key) {
    // Degenerate-key guard: some pipeline types (observed: ray tracing on driver
    // 621.14 — VK5 spike) get an all-zero key back from vkGetPipelineKeyKHR.
    // Such keys cannot address entries; the pipeline type bypasses the cache
    // entirely (plain creation) until the driver produces usable keys.
    if (key.keySize == 0) {
        return false;
    }
    for (std::uint32_t i = 0; i < key.keySize && i < VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR; ++i) {
        if (key.key[i] != 0U) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::string keyHex(const VkPipelineBinaryKeyKHR& key) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(static_cast<size_t>(key.keySize) * 2U);
    for (std::uint32_t i = 0; i < key.keySize && i < VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR; ++i) {
        out.push_back(kDigits[key.key[i] >> 4U]);
        out.push_back(kDigits[key.key[i] & 0x0FU]);
    }
    return out;
}

/// Bounds-checked little-endian reader over an in-memory blob.
class BlobReader {
  public:
    explicit BlobReader(std::vector<std::uint8_t> bytes) : m_bytes(std::move(bytes)) {}

    [[nodiscard]] bool readU32(std::uint32_t& out) { return readWord(out); }
    [[nodiscard]] bool readU64(std::uint64_t& out) { return readWord(out); }
    [[nodiscard]] bool readBytes(std::vector<std::uint8_t>& out, size_t count) {
        if (m_offset + count > m_bytes.size()) {
            return false;
        }
        out.assign(m_bytes.begin() + static_cast<ptrdiff_t>(m_offset),
                   m_bytes.begin() + static_cast<ptrdiff_t>(m_offset + count));
        m_offset += count;
        return true;
    }

  private:
    template <typename T> [[nodiscard]] bool readWord(T& out) {
        if (m_offset + sizeof(T) > m_bytes.size()) {
            return false;
        }
        std::memcpy(&out, m_bytes.data() + m_offset, sizeof(T));
        m_offset += sizeof(T);
        return true;
    }

    std::vector<std::uint8_t> m_bytes;
    size_t m_offset = 0;
};

void appendU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.insert(out.end(),
               reinterpret_cast<const std::uint8_t*>(&value),
               reinterpret_cast<const std::uint8_t*>(&value) + sizeof(value));
}

void appendU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    out.insert(out.end(),
               reinterpret_cast<const std::uint8_t*>(&value),
               reinterpret_cast<const std::uint8_t*>(&value) + sizeof(value));
}

void writeFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        Logger::error("PipelineBinaryCache: cannot write '{}' (the cache stays capture-only)", path.string());
        return;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

/// Appends `node` at the tail of the pNext chain whose first slot is `headSlot`;
/// returns the previous tail node (null when the chain was empty).
[[nodiscard]] const void* appendNode(const void*& headSlot, void* node) {
    if (headSlot == nullptr) {
        headSlot = node;
        return nullptr;
    }
    auto* tail = static_cast<VkBaseOutStructure*>(const_cast<void*>(headSlot));
    while (tail->pNext != nullptr) {
        tail = tail->pNext;
    }
    tail->pNext = static_cast<VkBaseOutStructure*>(node);
    return tail;
}

/// Inverse of appendNode — restores the chain so a fallback creation sees the
/// caller's original pNext.
void detachNode(const void*& headSlot, void* node, const void* prevTail) {
    if (prevTail != nullptr) {
        static_cast<VkBaseOutStructure*>(const_cast<void*>(prevTail))->pNext = nullptr;
    } else if (headSlot == node) {
        headSlot = nullptr;
    }
}

[[nodiscard]] std::filesystem::path entryPath(const std::filesystem::path& directory,
                                              const VkPipelineBinaryKeyKHR& pipelineKey) {
    return directory / (keyHex(pipelineKey) + kEntryExtension);
}

} // namespace

PipelineBinaryCache::PipelineBinaryCache(std::filesystem::path directory) : m_directory(std::move(directory)) {}

PipelineBinaryCache::PipelineBinaryCache(PipelineBinaryCache&&) noexcept = default;
PipelineBinaryCache& PipelineBinaryCache::operator=(PipelineBinaryCache&&) noexcept = default;

std::expected<PipelineBinaryCache, std::string> PipelineBinaryCache::create(const DeviceContext& ctx,
                                                                            std::filesystem::path directory) {
    if (!ctx.pipelineBinarySupported || vkGetPipelineKeyKHR == nullptr) {
        return std::unexpected("VK_KHR_pipeline_binary is not enabled on this device");
    }

    std::error_code fsError;
    std::filesystem::create_directories(directory, fsError);
    if (fsError) {
        return std::unexpected("cannot create cache directory '" + directory.string() + "': " + fsError.message());
    }

    // The driver's CURRENT global key — the validity anchor for the whole cache.
    VkPipelineBinaryKeyKHR globalKey = makeKey();
    if (vkGetPipelineKeyKHR(ctx.device, nullptr, &globalKey) != VK_SUCCESS) {
        return std::unexpected("vkGetPipelineKeyKHR (global key) failed");
    }

    // Reconcile with the stored key: a driver/feature/layer change invalidates
    // every cached blob → prune (all pipelines re-capture on this first run).
    std::uint32_t pruned = 0;
    std::ifstream globalFile(directory / kGlobalKeyFileName, std::ios::binary);
    bool storedValid = false;
    if (globalFile) {
        std::uint32_t storedSize = 0;
        globalFile.read(reinterpret_cast<char*>(&storedSize), sizeof(storedSize));
        if (globalFile && storedSize > 0 && storedSize <= VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR) {
            std::vector<std::uint8_t> stored(storedSize);
            globalFile.read(reinterpret_cast<char*>(stored.data()), static_cast<std::streamsize>(stored.size()));
            storedValid = static_cast<bool>(globalFile) && storedSize == globalKey.keySize &&
                          std::memcmp(stored.data(), globalKey.key, globalKey.keySize) == 0;
        }
    }
    if (!storedValid) {
        for (const auto& item : std::filesystem::directory_iterator(directory, fsError)) {
            if (item.is_regular_file() && item.path().extension() == kEntryExtension) {
                std::filesystem::remove(item.path(), fsError);
                ++pruned;
            }
        }
        fsError.clear();
    }

    std::vector<std::uint8_t> globalBytes;
    appendU32(globalBytes, globalKey.keySize);
    globalBytes.insert(globalBytes.end(), globalKey.key, globalKey.key + globalKey.keySize);
    writeFile(directory / kGlobalKeyFileName, globalBytes);

    PipelineBinaryCache cache{std::move(directory)};
    cache.m_receipts.pruned = pruned;
    if (pruned != 0U) {
        Logger::info("PipelineBinaryCache: pruned {} stale entrie(s) (global key changed)", pruned);
    }
    return cache;
}

std::expected<VkPipeline, VkResult> PipelineBinaryCache::computePipeline(const DeviceContext& ctx,
                                                                         VkComputePipelineCreateInfo info,
                                                                         const char* debugName) {
    if (ctx.pipelineBinaryCache == nullptr) {
        VkPipeline pipeline{};
        const VkResult result = vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
        if (result != VK_SUCCESS) {
            return std::unexpected(result);
        }
        return pipeline;
    }
    return ctx.pipelineBinaryCache->computePipelineImpl(ctx, info, debugName);
}

std::expected<VkPipeline, VkResult> PipelineBinaryCache::graphicsPipeline(const DeviceContext& ctx,
                                                                          VkGraphicsPipelineCreateInfo info,
                                                                          const char* debugName) {
    if (ctx.pipelineBinaryCache == nullptr) {
        VkPipeline pipeline{};
        const VkResult result = vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
        if (result != VK_SUCCESS) {
            return std::unexpected(result);
        }
        return pipeline;
    }
    return ctx.pipelineBinaryCache->graphicsPipelineImpl(ctx, info, debugName);
}

std::expected<VkPipeline, VkResult> PipelineBinaryCache::rayTracingPipeline(const DeviceContext& ctx,
                                                                            VkRayTracingPipelineCreateInfoKHR info,
                                                                            const char* debugName) {
    if (ctx.pipelineBinaryCache == nullptr) {
        VkPipeline pipeline{};
        const VkResult result =
            vkCreateRayTracingPipelinesKHR(ctx.device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
        if (result != VK_SUCCESS) {
            return std::unexpected(result);
        }
        return pipeline;
    }
    return ctx.pipelineBinaryCache->rayTracingPipelineImpl(ctx, info, debugName);
}

std::expected<std::vector<VkPipelineBinaryKHR>, std::string>
PipelineBinaryCache::createBinaryHandlesFromEntry(const DeviceContext& ctx,
                                                  const VkPipelineBinaryKeyKHR& pipelineKey) const {
    std::ifstream file(entryPath(m_directory, pipelineKey), std::ios::binary);
    if (!file) {
        return std::unexpected("no cached entry");
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    BlobReader reader(std::move(bytes));

    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t blobCount = 0;
    if (!reader.readU32(magic) || magic != kEntryMagic || !reader.readU32(version) || version != kEntryVersion ||
        !reader.readU32(blobCount) || blobCount == 0) {
        return std::unexpected("entry header invalid");
    }

    std::vector<EntryBlob> blobs;
    blobs.reserve(blobCount);
    for (std::uint32_t i = 0; i < blobCount; ++i) {
        EntryBlob blob;
        std::uint32_t keySize = 0;
        std::uint64_t dataSize = 0;
        if (!reader.readU32(keySize) || keySize == 0 || keySize > VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR ||
            !reader.readBytes(blob.data, keySize) || !reader.readU64(dataSize) || dataSize == 0 ||
            dataSize > (std::uint64_t{1} << 31U)) {
            return std::unexpected("entry record invalid");
        }
        blob.key = makeKey();
        blob.key.keySize = keySize;
        std::memcpy(blob.key.key, blob.data.data(), keySize);
        blob.data.clear();
        if (!reader.readBytes(blob.data, static_cast<size_t>(dataSize))) {
            return std::unexpected("entry data truncated");
        }
        blobs.push_back(std::move(blob));
    }

    std::vector<VkPipelineBinaryKeyKHR> keys;
    std::vector<VkPipelineBinaryDataKHR> datas;
    keys.reserve(blobs.size());
    datas.reserve(blobs.size());
    for (EntryBlob& blob : blobs) {
        keys.push_back(blob.key);
        datas.push_back(VkPipelineBinaryDataKHR{.dataSize = blob.data.size(), .pData = blob.data.data()});
    }
    const VkPipelineBinaryKeysAndDataKHR keysAndData{.binaryCount = static_cast<std::uint32_t>(keys.size()),
                                                     .pPipelineBinaryKeys = keys.data(),
                                                     .pPipelineBinaryData = datas.data()};
    const VkPipelineBinaryCreateInfoKHR createInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR,
                                                   .pNext = nullptr,
                                                   .pKeysAndDataInfo = &keysAndData,
                                                   .pipeline = VK_NULL_HANDLE,
                                                   .pPipelineCreateInfo = nullptr};
    VkPipelineBinaryHandlesInfoKHR handles{.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR,
                                           .pNext = nullptr,
                                           .pipelineBinaryCount = 0,
                                           .pPipelineBinaries = nullptr};
    if (vkCreatePipelineBinariesKHR(ctx.device, &createInfo, nullptr, &handles) != VK_SUCCESS) {
        return std::unexpected("vkCreatePipelineBinariesKHR (count query from data)");
    }
    std::vector<VkPipelineBinaryKHR> binaries(handles.pipelineBinaryCount);
    handles.pPipelineBinaries = binaries.data();
    if (handles.pipelineBinaryCount == 0 ||
        vkCreatePipelineBinariesKHR(ctx.device, &createInfo, nullptr, &handles) != VK_SUCCESS) {
        return std::unexpected("vkCreatePipelineBinariesKHR (handles from data)");
    }
    return binaries;
}

void PipelineBinaryCache::storeFromPipeline(const DeviceContext& ctx,
                                            VkPipeline pipeline,
                                            const VkPipelineBinaryKeyKHR& pipelineKey,
                                            const char* debugName) const {
    const VkPipelineBinaryCreateInfoKHR createInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR,
                                                   .pNext = nullptr,
                                                   .pKeysAndDataInfo = nullptr,
                                                   .pipeline = pipeline,
                                                   .pPipelineCreateInfo = nullptr};
    VkPipelineBinaryHandlesInfoKHR handles{.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR,
                                           .pNext = nullptr,
                                           .pipelineBinaryCount = 0,
                                           .pPipelineBinaries = nullptr};
    if (vkCreatePipelineBinariesKHR(ctx.device, &createInfo, nullptr, &handles) != VK_SUCCESS) {
        Logger::error("PipelineBinaryCache: '{}' capture count query failed (cache stays cold)", debugName);
        return;
    }
    std::vector<VkPipelineBinaryKHR> binaries(handles.pipelineBinaryCount);
    handles.pPipelineBinaries = binaries.data();
    if (handles.pipelineBinaryCount == 0 ||
        vkCreatePipelineBinariesKHR(ctx.device, &createInfo, nullptr, &handles) != VK_SUCCESS) {
        Logger::error("PipelineBinaryCache: '{}' capture failed (cache stays cold)", debugName);
        return;
    }

    std::vector<EntryBlob> blobs;
    blobs.reserve(binaries.size());
    bool complete = true;
    std::vector<std::uint8_t> fileBytes;
    appendU32(fileBytes, kEntryMagic);
    appendU32(fileBytes, kEntryVersion);
    appendU32(fileBytes, static_cast<std::uint32_t>(binaries.size()));
    for (const VkPipelineBinaryKHR binary : binaries) {
        const VkPipelineBinaryDataInfoKHR dataInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR, .pNext = nullptr, .pipelineBinary = binary};
        VkPipelineBinaryKeyKHR key = makeKey();
        size_t dataSize = 0;
        // Two-call pattern; a valid key pointer is required on BOTH calls (the
        // official Khronos sample contract).
        if (vkGetPipelineBinaryDataKHR(ctx.device, &dataInfo, &key, &dataSize, nullptr) != VK_SUCCESS ||
            dataSize == 0) {
            complete = false;
            break;
        }
        EntryBlob blob;
        blob.key = key;
        blob.data.resize(dataSize);
        if (vkGetPipelineBinaryDataKHR(ctx.device, &dataInfo, &key, &dataSize, blob.data.data()) != VK_SUCCESS) {
            complete = false;
            break;
        }
        appendU32(fileBytes, key.keySize);
        fileBytes.insert(fileBytes.end(), key.key, key.key + key.keySize);
        appendU64(fileBytes, blob.data.size());
        fileBytes.insert(fileBytes.end(), blob.data.begin(), blob.data.end());
        blobs.push_back(std::move(blob));
    }
    for (const VkPipelineBinaryKHR binary : binaries) {
        vkDestroyPipelineBinaryKHR(ctx.device, binary, nullptr);
    }
    const VkReleaseCapturedPipelineDataInfoKHR releaseInfo{
        .sType = VK_STRUCTURE_TYPE_RELEASE_CAPTURED_PIPELINE_DATA_INFO_KHR, .pNext = nullptr, .pipeline = pipeline};
    vkReleaseCapturedPipelineDataKHR(ctx.device, &releaseInfo, nullptr);

    if (!complete) {
        Logger::error("PipelineBinaryCache: '{}' capture incomplete (cache stays cold)", debugName);
        return;
    }
    writeFile(entryPath(m_directory, pipelineKey), fileBytes);
    Logger::info("PipelineBinaryCache: '{}' captured {} blob(s), {} bytes (key {})",
                 debugName,
                 blobs.size(),
                 fileBytes.size(),
                 keyHex(pipelineKey));
}

std::expected<VkPipeline, VkResult> PipelineBinaryCache::computePipelineImpl(const DeviceContext& ctx,
                                                                             VkComputePipelineCreateInfo info,
                                                                             const char* debugName) const {
    // Driver pipeline key for this create-info (the implementation skips state
    // that cannot affect the binary — no application-side hashing to drift).
    const VkPipelineCreateInfoKHR keyQuery{.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_INFO_KHR, .pNext = &info};
    VkPipelineBinaryKeyKHR pipelineKey = makeKey();
    const VkResult keyResult = vkGetPipelineKeyKHR(ctx.device, &keyQuery, &pipelineKey);
    if (keyResult != VK_SUCCESS) {
        Logger::error("PipelineBinaryCache: key query failed for '{}' (creating uncached)", debugName);
    } else if (!usableKey(pipelineKey)) {
        // Expected bypass for pipeline types the driver gives no key for
        // (ray tracing on driver 621.14 — VK5 spike receipt).
        Logger::info("PipelineBinaryCache: '{}' bypassed (no usable driver key for this pipeline type)", debugName);
    }
    if (keyResult != VK_SUCCESS || !usableKey(pipelineKey)) {
        VkPipeline pipeline{};
        const VkResult result = vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
        if (result != VK_SUCCESS) {
            return std::unexpected(result);
        }
        return pipeline;
    }

    // HIT arm: binary handles from the cached entry, creation from them.
    auto binaries = createBinaryHandlesFromEntry(ctx, pipelineKey);
    if (binaries.has_value()) {
        VkPipelineBinaryInfoKHR binaryInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR,
                                           .pNext = nullptr,
                                           .binaryCount = static_cast<std::uint32_t>(binaries->size()),
                                           .pPipelineBinaries = binaries->data()};
        const void* prevTail = appendNode(info.pNext, &binaryInfo);
        VkPipeline pipeline{};
        const VkResult result = vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
        detachNode(info.pNext, &binaryInfo, prevTail);
        for (const VkPipelineBinaryKHR binary : *binaries) {
            vkDestroyPipelineBinaryKHR(ctx.device, binary, nullptr);
        }
        if (result == VK_SUCCESS) {
            ++m_receipts.hits;
            Logger::info("PipelineBinaryCache: '{}' created from {} cached blob(s)", debugName, binaries->size());
            return pipeline;
        }
        Logger::error("PipelineBinaryCache: '{}' failed from cached binary (VkResult {}) — re-capturing",
                      debugName,
                      static_cast<int>(result));
        std::error_code removeError;
        std::filesystem::remove(entryPath(m_directory, pipelineKey), removeError);
    } else {
        // Entry missing or corrupt — a corrupt entry is removed (self-heal).
        std::error_code removeError;
        std::filesystem::remove(entryPath(m_directory, pipelineKey), removeError);
        if (binaries.error() != "no cached entry") {
            Logger::error(
                "PipelineBinaryCache: entry for '{}' unreadable ({}) — re-capturing", debugName, binaries.error());
        }
    }

    // MISS arm (and the hit-arm fallback): capture + store. The flags2 chain
    // replaces the legacy 32-bit flags (maintenance5), so the caller's base
    // flags ride inside it.
    VkPipelineCreateFlags2CreateInfoKHR flags2{.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR,
                                               .pNext = nullptr,
                                               .flags = static_cast<VkPipelineCreateFlags2KHR>(info.flags) |
                                                        VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR};
    const VkPipelineCreateFlags originalFlags = info.flags;
    info.flags = 0;
    const void* prevTail = appendNode(info.pNext, &flags2);
    VkPipeline pipeline{};
    const VkResult result = vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    info.flags = originalFlags;
    detachNode(info.pNext, &flags2, prevTail);
    if (result != VK_SUCCESS) {
        return std::unexpected(result);
    }
    ++m_receipts.misses;
    storeFromPipeline(ctx, pipeline, pipelineKey, debugName);
    return pipeline;
}

std::expected<VkPipeline, VkResult> PipelineBinaryCache::graphicsPipelineImpl(const DeviceContext& ctx,
                                                                              VkGraphicsPipelineCreateInfo info,
                                                                              const char* debugName) const {
    const VkPipelineCreateInfoKHR keyQuery{.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_INFO_KHR, .pNext = &info};
    VkPipelineBinaryKeyKHR pipelineKey = makeKey();
    const VkResult keyResult = vkGetPipelineKeyKHR(ctx.device, &keyQuery, &pipelineKey);
    if (keyResult != VK_SUCCESS) {
        Logger::error("PipelineBinaryCache: key query failed for '{}' (creating uncached)", debugName);
    } else if (!usableKey(pipelineKey)) {
        // Expected bypass for pipeline types the driver gives no key for
        // (ray tracing on driver 621.14 — VK5 spike receipt).
        Logger::info("PipelineBinaryCache: '{}' bypassed (no usable driver key for this pipeline type)", debugName);
    }
    if (keyResult != VK_SUCCESS || !usableKey(pipelineKey)) {
        VkPipeline pipeline{};
        const VkResult result = vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
        if (result != VK_SUCCESS) {
            return std::unexpected(result);
        }
        return pipeline;
    }

    auto binaries = createBinaryHandlesFromEntry(ctx, pipelineKey);
    if (binaries.has_value()) {
        VkPipelineBinaryInfoKHR binaryInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR,
                                           .pNext = nullptr,
                                           .binaryCount = static_cast<std::uint32_t>(binaries->size()),
                                           .pPipelineBinaries = binaries->data()};
        const void* prevTail = appendNode(info.pNext, &binaryInfo);
        VkPipeline pipeline{};
        const VkResult result = vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
        detachNode(info.pNext, &binaryInfo, prevTail);
        for (const VkPipelineBinaryKHR binary : *binaries) {
            vkDestroyPipelineBinaryKHR(ctx.device, binary, nullptr);
        }
        if (result == VK_SUCCESS) {
            ++m_receipts.hits;
            Logger::info("PipelineBinaryCache: '{}' created from {} cached blob(s)", debugName, binaries->size());
            return pipeline;
        }
        Logger::error("PipelineBinaryCache: '{}' failed from cached binary (VkResult {}) — re-capturing",
                      debugName,
                      static_cast<int>(result));
        std::error_code removeError;
        std::filesystem::remove(entryPath(m_directory, pipelineKey), removeError);
    }

    VkPipelineCreateFlags2CreateInfoKHR flags2{.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR,
                                               .pNext = nullptr,
                                               .flags = static_cast<VkPipelineCreateFlags2KHR>(info.flags) |
                                                        VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR};
    const VkPipelineCreateFlags originalFlags = info.flags;
    info.flags = 0;
    const void* prevTail = appendNode(info.pNext, &flags2);
    VkPipeline pipeline{};
    const VkResult result = vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    info.flags = originalFlags;
    detachNode(info.pNext, &flags2, prevTail);
    if (result != VK_SUCCESS) {
        return std::unexpected(result);
    }
    ++m_receipts.misses;
    storeFromPipeline(ctx, pipeline, pipelineKey, debugName);
    return pipeline;
}

std::expected<VkPipeline, VkResult> PipelineBinaryCache::rayTracingPipelineImpl(const DeviceContext& ctx,
                                                                                VkRayTracingPipelineCreateInfoKHR info,
                                                                                const char* debugName) const {
    const VkPipelineCreateInfoKHR keyQuery{.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_INFO_KHR, .pNext = &info};
    VkPipelineBinaryKeyKHR pipelineKey = makeKey();
    const VkResult keyResult = vkGetPipelineKeyKHR(ctx.device, &keyQuery, &pipelineKey);
    if (keyResult != VK_SUCCESS) {
        Logger::error("PipelineBinaryCache: key query failed for '{}' (creating uncached)", debugName);
    } else if (!usableKey(pipelineKey)) {
        // Expected bypass for pipeline types the driver gives no key for
        // (ray tracing on driver 621.14 — VK5 spike receipt).
        Logger::info("PipelineBinaryCache: '{}' bypassed (no usable driver key for this pipeline type)", debugName);
    }
    if (keyResult != VK_SUCCESS || !usableKey(pipelineKey)) {
        VkPipeline pipeline{};
        const VkResult result =
            vkCreateRayTracingPipelinesKHR(ctx.device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
        if (result != VK_SUCCESS) {
            return std::unexpected(result);
        }
        return pipeline;
    }

    auto binaries = createBinaryHandlesFromEntry(ctx, pipelineKey);
    if (binaries.has_value()) {
        VkPipelineBinaryInfoKHR binaryInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR,
                                           .pNext = nullptr,
                                           .binaryCount = static_cast<std::uint32_t>(binaries->size()),
                                           .pPipelineBinaries = binaries->data()};
        const void* prevTail = appendNode(info.pNext, &binaryInfo);
        VkPipeline pipeline{};
        const VkResult result =
            vkCreateRayTracingPipelinesKHR(ctx.device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
        detachNode(info.pNext, &binaryInfo, prevTail);
        for (const VkPipelineBinaryKHR binary : *binaries) {
            vkDestroyPipelineBinaryKHR(ctx.device, binary, nullptr);
        }
        if (result == VK_SUCCESS) {
            ++m_receipts.hits;
            Logger::info("PipelineBinaryCache: '{}' created from {} cached blob(s)", debugName, binaries->size());
            return pipeline;
        }
        Logger::error("PipelineBinaryCache: '{}' failed from cached binary (VkResult {}) — re-capturing",
                      debugName,
                      static_cast<int>(result));
        std::error_code removeError;
        std::filesystem::remove(entryPath(m_directory, pipelineKey), removeError);
    }

    VkPipelineCreateFlags2CreateInfoKHR flags2{.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR,
                                               .pNext = nullptr,
                                               .flags = static_cast<VkPipelineCreateFlags2KHR>(info.flags) |
                                                        VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR};
    const VkPipelineCreateFlags originalFlags = info.flags;
    info.flags = 0;
    const void* prevTail = appendNode(info.pNext, &flags2);
    VkPipeline pipeline{};
    const VkResult result =
        vkCreateRayTracingPipelinesKHR(ctx.device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    info.flags = originalFlags;
    detachNode(info.pNext, &flags2, prevTail);
    if (result != VK_SUCCESS) {
        return std::unexpected(result);
    }
    ++m_receipts.misses;
    storeFromPipeline(ctx, pipeline, pipelineKey, debugName);
    return pipeline;
}

} // namespace harmonia::core
