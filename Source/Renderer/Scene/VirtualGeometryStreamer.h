#pragma once

#include "VirtualGeometryCache.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace Halcyon::Renderer::Scene
{

enum class VirtualGeometryPageState : std::uint8_t
{
    Unloaded,
    Requested,
    Loading,
    ReadyCPU,
    Uploading,
    Resident,
    EvictPending,
    Failed,
};

struct VirtualGeometryStreamingConfig
{
    std::uint64_t gpuPagePoolBytes = 1ull * 1024ull * 1024ull * 1024ull;
    std::uint64_t cpuStagingBudgetBytes = 256ull * 1024ull * 1024ull;
    std::uint64_t maxUploadBytesPerFrame = 16ull * 1024ull * 1024ull;
    std::uint32_t maxUploadPagesPerFrame = 128u;
    std::uint32_t maxReadRetries = 2u;
    std::uint32_t protectedFrameCount = 2u;
};

struct VirtualGeometryReadyPage
{
    std::uint32_t pageIndex = 0;
    std::uint32_t generation = 0;
    std::vector<std::byte> bytes;
};

struct VirtualGeometryStreamingStats
{
    std::uint64_t requestedPages = 0;
    std::uint64_t loadedPages = 0;
    std::uint64_t failedPages = 0;
    std::uint64_t retriedReads = 0;
    std::uint64_t evictedPages = 0;
    std::uint64_t readyBytes = 0;
    std::uint64_t usageTouches = 0;
    std::uint64_t evictionFrameProtected = 0;
    std::uint64_t evictionTimelineBlocked = 0;
    std::uint32_t queuedPages = 0;
    std::uint32_t residentPages = 0;
    std::uint32_t residentPagePeak = 0;
};

class VirtualGeometryStreamer final
{
public:
    VirtualGeometryStreamer(const VirtualGeometryStreamer&) = delete;
    VirtualGeometryStreamer& operator=(const VirtualGeometryStreamer&) = delete;
    ~VirtualGeometryStreamer();

    [[nodiscard]] static Halcyon::Result<std::unique_ptr<VirtualGeometryStreamer>> open(
        const std::filesystem::path& path,
        const Sha256Digest* expectedSourceHash = nullptr,
        const VirtualGeometryCacheOptions* expectedOptions = nullptr,
        const VirtualGeometryStreamingConfig& config = {});

    [[nodiscard]] const VirtualGeometryCacheMetadata& metadata() const noexcept;
    [[nodiscard]] const VirtualGeometryStreamingConfig& config() const noexcept;

    // Higher priority values are loaded first. Duplicate requests are folded
    // into the existing state transition and never enqueue duplicate IO.
    [[nodiscard]] bool requestPage(
        std::uint32_t pageIndex, float priority, std::uint64_t frameIndex);

    // Moves CPU-ready pages into Uploading state. The caller publishes them to
    // the GPU and completes the transition with markResident().
    [[nodiscard]] std::vector<VirtualGeometryReadyPage> takeReadyPages(
        std::uint64_t maxBytes = 0u, std::uint32_t maxPages = 0u);

    [[nodiscard]] bool markResident(std::uint32_t pageIndex,
        std::uint32_t generation, std::uint64_t frameIndex,
        std::uint64_t lastUseTimeline);
    [[nodiscard]] bool abandonUpload(std::uint32_t pageIndex,
        std::uint32_t generation);
    [[nodiscard]] bool touchResident(std::uint32_t pageIndex,
        std::uint32_t generation, std::uint64_t frameIndex,
        std::uint64_t lastUseTimeline);

    // Selects the least-recently-used page that is no longer referenced by
    // the GPU. Root pages and the most recent protected frames are skipped.
    [[nodiscard]] std::uint32_t beginEviction(
        std::uint64_t completedTimeline, std::uint64_t frameIndex);
    [[nodiscard]] bool finishEviction(
        std::uint32_t pageIndex, std::uint32_t generation);

    [[nodiscard]] VirtualGeometryPageState pageState(std::uint32_t pageIndex) const;
    [[nodiscard]] std::uint32_t pageGeneration(std::uint32_t pageIndex) const;
    [[nodiscard]] bool waitForPageState(std::uint32_t pageIndex,
        VirtualGeometryPageState state, std::chrono::milliseconds timeout) const;
    [[nodiscard]] VirtualGeometryStreamingStats stats() const;

private:
    struct Impl;
    explicit VirtualGeometryStreamer(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace Halcyon::Renderer::Scene
