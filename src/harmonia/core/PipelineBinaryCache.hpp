#ifndef HARMONIA_CORE_PIPELINEBINARYCACHE_HPP
#define HARMONIA_CORE_PIPELINEBINARYCACHE_HPP

#include <volk/volk.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

#include "harmonia/DeviceContext.hpp"

namespace harmonia::core {

/// App-managed explicit pipeline-binary cache (VK_KHR_pipeline_binary — VK5).
///
/// Replaces the opaque VkPipelineCache with driver-produced pipeline binaries the
/// app persists itself: a pipeline is created once with the capture flag, its
/// binaries are written to disk, and later runs create the same pipeline FROM
/// those binaries (no compilation). The win is cross-session — the measured
/// cold compile of one small compute pipeline is ~90 ms, ~0.1 ms from its
/// captured binary (PLAN §5.8 VK5 probe receipts).
///
/// Cache-validity contract (spec + Khronos blog; receipt-tested in
/// tests/component/test_pipeline_binary_cache.cpp): the driver's GLOBAL key
/// (vkGetPipelineKeyKHR with a null create-info) changes whenever binary
/// contents can change — a driver update, a feature/extension change, enabled
/// layers. On create() the stored global key is reconciled with the driver's;
/// on mismatch every cached entry is pruned (all pipelines re-capture on the
/// new driver's first run). THIS is why the cache never needs manual
/// invalidation logic of its own.
///
/// Failure policy: best-effort by design. Every cache-side failure (disk I/O,
/// decode, stale blob, binary-recreation failure) is logged and falls back to
/// from-scratch creation with re-capture — the only error that can reach the
/// caller is a genuine pipeline creation failure, reported as the same VkResult
/// an uncached path would produce. Nothing here biases the image: a pipeline
/// built from its own captured binary IS that pipeline (convergence-to-Hyperion
/// litmus passes by construction).
///
/// Identity: entries are keyed on the DRIVER's pipeline key
/// (vkGetPipelineKeyKHR over the caller's create-info) — the implementation
/// skips state that cannot affect the binary, so no application-side hashing
/// exists to drift. Identical create-infos may dedup onto one binary
/// (spec-legal; the Vk5 probe receipts log it).
///
/// Threading: init-time use, single-threaded (matches every current consumer).
///
/// Opt-in: the App creates the cache when HARMONIA_PIPELINE_BINARY_CACHE=<dir>
/// is set and the extension is present; DeviceContext::pipelineBinaryCache then
/// exposes it to the creation sites. Without the env var nothing changes.
class PipelineBinaryCache {
  public:
    /// Opens (or seeds) the cache directory: reconciles the stored global key
    /// with the driver's current one (mismatch → prune all entries), then
    /// writes the driver's key back so subsequent runs compare against it.
    [[nodiscard]] static std::expected<PipelineBinaryCache, std::string> create(const DeviceContext& ctx,
                                                                                std::filesystem::path directory);

    PipelineBinaryCache(PipelineBinaryCache&&) noexcept;
    PipelineBinaryCache& operator=(PipelineBinaryCache&&) noexcept;
    ~PipelineBinaryCache() noexcept = default;

    PipelineBinaryCache(const PipelineBinaryCache&) = delete;
    PipelineBinaryCache& operator=(const PipelineBinaryCache&) = delete;

    /// Compute pipeline via the cache: create-from-binary on a key hit, create
    /// with capture + store on a miss (falling back to plain creation on any
    /// cache-side failure). `info` is taken by value — the cache may append
    /// nodes to its pNext chain (binary info on a hit, capture flags on a miss)
    /// and always restores the chain before returning or falling back.
    [[nodiscard]] static std::expected<VkPipeline, VkResult>
    computePipeline(const DeviceContext& ctx, VkComputePipelineCreateInfo info, const char* debugName);

    /// Graphics variant (same contract; appended nodes go to the pNext tail).
    [[nodiscard]] static std::expected<VkPipeline, VkResult>
    graphicsPipeline(const DeviceContext& ctx, VkGraphicsPipelineCreateInfo info, const char* debugName);

    /// Receipt counters for tests/diagnostics: cache hits, misses (captured),
    /// and entries pruned by global-key changes.
    struct Receipts {
        std::uint32_t hits = 0;
        std::uint32_t misses = 0;
        std::uint32_t pruned = 0;
    };

    [[nodiscard]] const Receipts& receipts() const noexcept { return m_receipts; }

  private:
    PipelineBinaryCache(std::filesystem::path directory);

    /// Serialized blob (binary key + bytes) as persisted per cached pipeline.
    struct EntryBlob {
        VkPipelineBinaryKeyKHR key{};
        std::vector<std::uint8_t> data;
    };

    [[nodiscard]] std::expected<VkPipeline, VkResult>
    computePipelineImpl(const DeviceContext& ctx, VkComputePipelineCreateInfo info, const char* debugName) const;
    [[nodiscard]] std::expected<VkPipeline, VkResult>
    graphicsPipelineImpl(const DeviceContext& ctx, VkGraphicsPipelineCreateInfo info, const char* debugName) const;

    /// Rebuilds VkPipelineBinaryKHR handles from the on-disk entry for the given
    /// pipeline key. Any parse/recreation failure removes the entry (self-heal)
    /// and reports the error — the caller falls back to capture.
    [[nodiscard]] std::expected<std::vector<VkPipelineBinaryKHR>, std::string>
    createBinaryHandlesFromEntry(const DeviceContext& ctx, const VkPipelineBinaryKeyKHR& pipelineKey) const;

    /// Captures the driver binaries of a CAPTURE_DATA-created pipeline into the
    /// entry file. Best-effort: failures are logged and leave the pipeline valid.
    void storeFromPipeline(const DeviceContext& ctx,
                           VkPipeline pipeline,
                           const VkPipelineBinaryKeyKHR& pipelineKey,
                           const char* debugName) const;

    std::filesystem::path m_directory;
    mutable Receipts m_receipts{};
};

} // namespace harmonia::core

#endif // HARMONIA_CORE_PIPELINEBINARYCACHE_HPP
