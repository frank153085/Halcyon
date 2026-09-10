#include "VirtualGeometryStreamer.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <queue>
#include <stop_token>
#include <thread>
#include <utility>

namespace Halcyon::Renderer::Scene
{
namespace
{

constexpr std::uint32_t kInvalidPage = std::numeric_limits<std::uint32_t>::max();

struct PageRuntimeState
{
    VirtualGeometryPageState state = VirtualGeometryPageState::Unloaded;
    std::uint32_t generation = 1u;
    std::uint32_t retries = 0u;
    std::uint64_t lastUsedFrame = 0u;
    std::uint64_t lastUseTimeline = 0u;
};

struct QueuedRequest
{
    float priority = 0.0f;
    std::uint64_t sequence = 0u;
    std::uint64_t frameIndex = 0u;
    std::uint32_t pageIndex = 0u;
};

struct RequestOrder
{
    bool operator()(const QueuedRequest& left, const QueuedRequest& right) const noexcept
    {
        if (left.priority != right.priority)
            return left.priority < right.priority;
        return left.sequence > right.sequence;
    }
};

struct ReadyItem
{
    std::uint32_t pageIndex = 0u;
    std::vector<std::byte> bytes;
};

} // namespace

struct VirtualGeometryStreamer::Impl
{
    std::filesystem::path path;
    VirtualGeometryCacheMetadata metadata;
    VirtualGeometryStreamingConfig config;
    mutable std::mutex mutex;
    mutable std::condition_variable_any condition;
    std::priority_queue<QueuedRequest, std::vector<QueuedRequest>, RequestOrder> requests;
    std::deque<ReadyItem> ready;
    std::vector<PageRuntimeState> pages;
    VirtualGeometryStreamingStats stats;
    std::uint64_t requestSequence = 0u;
    std::jthread worker;

    void workerMain(std::stop_token stopToken)
    {
        while (!stopToken.stop_requested())
        {
            QueuedRequest request;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, stopToken, [&]
                {
                    return !requests.empty();
                });
                if (stopToken.stop_requested())
                    break;
                request = requests.top();
                requests.pop();
                stats.queuedPages = static_cast<std::uint32_t>(requests.size());
                auto& page = pages[request.pageIndex];
                if (page.state != VirtualGeometryPageState::Requested)
                    continue;
                page.state = VirtualGeometryPageState::Loading;
                condition.notify_all();
            }

            auto loaded = readVirtualGeometryCachePage(path, metadata, request.pageIndex);
            std::unique_lock lock(mutex);
            auto& page = pages[request.pageIndex];
            if (!loaded)
            {
                if (page.retries < config.maxReadRetries)
                {
                    ++page.retries;
                    ++stats.retriedReads;
                    page.state = VirtualGeometryPageState::Requested;
                    request.sequence = requestSequence++;
                    requests.push(request);
                    stats.queuedPages = static_cast<std::uint32_t>(requests.size());
                }
                else
                {
                    page.state = VirtualGeometryPageState::Failed;
                    ++stats.failedPages;
                }
                condition.notify_all();
                continue;
            }

            const std::uint64_t byteCount = loaded->size();
            condition.wait(lock, stopToken, [&]
            {
                return stats.readyBytes + byteCount <= config.cpuStagingBudgetBytes;
            });
            if (stopToken.stop_requested())
                break;
            page.state = VirtualGeometryPageState::ReadyCPU;
            page.retries = 0u;
            stats.readyBytes += byteCount;
            ++stats.loadedPages;
            ready.push_back({request.pageIndex, std::move(loaded.value())});
            condition.notify_all();
        }
    }
};

VirtualGeometryStreamer::VirtualGeometryStreamer(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl))
{
}

VirtualGeometryStreamer::~VirtualGeometryStreamer()
{
    if (impl_ && impl_->worker.joinable())
    {
        impl_->worker.request_stop();
        impl_->condition.notify_all();
    }
}

Halcyon::Result<std::unique_ptr<VirtualGeometryStreamer>> VirtualGeometryStreamer::open(
    const std::filesystem::path& path, const Sha256Digest* expectedSourceHash,
    const VirtualGeometryCacheOptions* expectedOptions,
    const VirtualGeometryStreamingConfig& config)
{
    auto metadata = readVirtualGeometryCacheMetadata(
        path, expectedSourceHash, expectedOptions);
    if (!metadata)
        return Halcyon::Result<std::unique_ptr<VirtualGeometryStreamer>>::failure(
            metadata.error());
    if (config.cpuStagingBudgetBytes < metadata->options.pageSize ||
        config.gpuPagePoolBytes < metadata->options.pageSize ||
        config.maxUploadBytesPerFrame == 0u || config.maxUploadPagesPerFrame == 0u)
        return Halcyon::Result<std::unique_ptr<VirtualGeometryStreamer>>::failure(
            {Halcyon::ErrorCode::InvalidArgument,
                "streaming budgets must hold at least one cache page",
                "VirtualGeometryStreamer"});

    auto impl = std::make_unique<Impl>();
    impl->path = path;
    impl->metadata = std::move(metadata.value());
    impl->config = config;
    impl->pages.resize(impl->metadata.pages.size());

    for (const std::uint32_t pageIndex : impl->metadata.rootPageIndices)
    {
        Halcyon::Result<std::vector<std::byte>> root =
            readVirtualGeometryCachePage(path, impl->metadata, pageIndex);
        for (std::uint32_t retry = 0; !root && retry < config.maxReadRetries; ++retry)
        {
            ++impl->stats.retriedReads;
            root = readVirtualGeometryCachePage(path, impl->metadata, pageIndex);
        }
        if (!root)
            return Halcyon::Result<std::unique_ptr<VirtualGeometryStreamer>>::failure(
                {Halcyon::ErrorCode::Io, "unable to load required root geometry page",
                    path.string()});
        if (impl->stats.readyBytes + root->size() > config.cpuStagingBudgetBytes)
            return Halcyon::Result<std::unique_ptr<VirtualGeometryStreamer>>::failure(
                {Halcyon::ErrorCode::OutOfMemory,
                    "root geometry pages exceed the CPU staging budget", path.string()});
        impl->pages[pageIndex].state = VirtualGeometryPageState::ReadyCPU;
        impl->stats.readyBytes += root->size();
        ++impl->stats.loadedPages;
        impl->ready.push_back({pageIndex, std::move(root.value())});
    }

    impl->worker = std::jthread([raw = impl.get()](std::stop_token stopToken)
    {
        raw->workerMain(stopToken);
    });
    return Halcyon::Result<std::unique_ptr<VirtualGeometryStreamer>>::success(
        std::unique_ptr<VirtualGeometryStreamer>(
            new VirtualGeometryStreamer(std::move(impl))));
}

const VirtualGeometryCacheMetadata& VirtualGeometryStreamer::metadata() const noexcept
{
    return impl_->metadata;
}

const VirtualGeometryStreamingConfig& VirtualGeometryStreamer::config() const noexcept
{
    return impl_->config;
}

bool VirtualGeometryStreamer::requestPage(
    std::uint32_t pageIndex, float priority, std::uint64_t frameIndex)
{
    if (pageIndex >= impl_->pages.size() || !std::isfinite(priority))
        return false;
    std::lock_guard lock(impl_->mutex);
    auto& page = impl_->pages[pageIndex];
    if (page.state != VirtualGeometryPageState::Unloaded)
        return false;
    page.state = VirtualGeometryPageState::Requested;
    page.lastUsedFrame = frameIndex;
    impl_->requests.push({priority, impl_->requestSequence++, frameIndex, pageIndex});
    ++impl_->stats.requestedPages;
    impl_->stats.queuedPages = static_cast<std::uint32_t>(impl_->requests.size());
    impl_->condition.notify_all();
    return true;
}

std::vector<VirtualGeometryReadyPage> VirtualGeometryStreamer::takeReadyPages(
    std::uint64_t maxBytes, std::uint32_t maxPages)
{
    if (maxBytes == 0u)
        maxBytes = impl_->config.maxUploadBytesPerFrame;
    if (maxPages == 0u)
        maxPages = impl_->config.maxUploadPagesPerFrame;
    std::vector<VirtualGeometryReadyPage> result;
    std::lock_guard lock(impl_->mutex);
    std::uint64_t selectedBytes = 0u;
    while (!impl_->ready.empty() && result.size() < maxPages)
    {
        auto& front = impl_->ready.front();
        if (!result.empty() && selectedBytes + front.bytes.size() > maxBytes)
            break;
        auto& page = impl_->pages[front.pageIndex];
        if (page.state != VirtualGeometryPageState::ReadyCPU)
        {
            impl_->ready.pop_front();
            continue;
        }
        selectedBytes += front.bytes.size();
        impl_->stats.readyBytes -= front.bytes.size();
        page.state = VirtualGeometryPageState::Uploading;
        result.push_back({front.pageIndex, page.generation, std::move(front.bytes)});
        impl_->ready.pop_front();
    }
    impl_->condition.notify_all();
    return result;
}

bool VirtualGeometryStreamer::markResident(std::uint32_t pageIndex,
    std::uint32_t generation, std::uint64_t frameIndex, std::uint64_t lastUseTimeline)
{
    if (pageIndex >= impl_->pages.size())
        return false;
    std::lock_guard lock(impl_->mutex);
    auto& page = impl_->pages[pageIndex];
    if (page.state != VirtualGeometryPageState::Uploading ||
        page.generation != generation)
        return false;
    page.state = VirtualGeometryPageState::Resident;
    page.lastUsedFrame = frameIndex;
    page.lastUseTimeline = lastUseTimeline;
    ++impl_->stats.residentPages;
    impl_->condition.notify_all();
    return true;
}

bool VirtualGeometryStreamer::abandonUpload(
    std::uint32_t pageIndex, std::uint32_t generation)
{
    if (pageIndex >= impl_->pages.size())
        return false;
    std::lock_guard lock(impl_->mutex);
    auto& page = impl_->pages[pageIndex];
    if (page.state != VirtualGeometryPageState::Uploading ||
        page.generation != generation)
        return false;
    page.state = VirtualGeometryPageState::Unloaded;
    return true;
}

bool VirtualGeometryStreamer::touchResident(std::uint32_t pageIndex,
    std::uint32_t generation, std::uint64_t frameIndex, std::uint64_t lastUseTimeline)
{
    if (pageIndex >= impl_->pages.size())
        return false;
    std::lock_guard lock(impl_->mutex);
    auto& page = impl_->pages[pageIndex];
    if (page.state != VirtualGeometryPageState::Resident ||
        page.generation != generation)
        return false;
    page.lastUsedFrame = std::max(page.lastUsedFrame, frameIndex);
    page.lastUseTimeline = std::max(page.lastUseTimeline, lastUseTimeline);
    return true;
}

std::uint32_t VirtualGeometryStreamer::beginEviction(
    std::uint64_t completedTimeline, std::uint64_t frameIndex)
{
    std::lock_guard lock(impl_->mutex);
    std::uint32_t candidate = kInvalidPage;
    std::uint64_t oldestFrame = std::numeric_limits<std::uint64_t>::max();
    for (std::uint32_t pageIndex = 0u; pageIndex < impl_->pages.size(); ++pageIndex)
    {
        const auto& page = impl_->pages[pageIndex];
        if ((impl_->metadata.pages[pageIndex].flags &
                VirtualGeometryCachePagePinnedRoot) != 0u)
            continue;
        const bool protectedByFrame = page.lastUsedFrame > frameIndex ||
            frameIndex - page.lastUsedFrame <= impl_->config.protectedFrameCount;
        if (page.state == VirtualGeometryPageState::Resident &&
            page.lastUseTimeline <= completedTimeline && !protectedByFrame &&
            page.lastUsedFrame < oldestFrame)
        {
            candidate = pageIndex;
            oldestFrame = page.lastUsedFrame;
        }
    }
    if (candidate != kInvalidPage)
        impl_->pages[candidate].state = VirtualGeometryPageState::EvictPending;
    return candidate;
}

bool VirtualGeometryStreamer::finishEviction(
    std::uint32_t pageIndex, std::uint32_t generation)
{
    if (pageIndex >= impl_->pages.size())
        return false;
    std::lock_guard lock(impl_->mutex);
    auto& page = impl_->pages[pageIndex];
    if (page.state != VirtualGeometryPageState::EvictPending ||
        page.generation != generation)
        return false;
    page.state = VirtualGeometryPageState::Unloaded;
    page.generation = page.generation == std::numeric_limits<std::uint32_t>::max() ?
        1u : page.generation + 1u;
    --impl_->stats.residentPages;
    ++impl_->stats.evictedPages;
    return true;
}

VirtualGeometryPageState VirtualGeometryStreamer::pageState(std::uint32_t pageIndex) const
{
    if (pageIndex >= impl_->pages.size())
        return VirtualGeometryPageState::Failed;
    std::lock_guard lock(impl_->mutex);
    return impl_->pages[pageIndex].state;
}

std::uint32_t VirtualGeometryStreamer::pageGeneration(std::uint32_t pageIndex) const
{
    if (pageIndex >= impl_->pages.size())
        return 0u;
    std::lock_guard lock(impl_->mutex);
    return impl_->pages[pageIndex].generation;
}

bool VirtualGeometryStreamer::waitForPageState(std::uint32_t pageIndex,
    VirtualGeometryPageState state, std::chrono::milliseconds timeout) const
{
    if (pageIndex >= impl_->pages.size())
        return false;
    std::unique_lock lock(impl_->mutex);
    return impl_->condition.wait_for(lock, timeout, [&]
    {
        return impl_->pages[pageIndex].state == state;
    });
}

VirtualGeometryStreamingStats VirtualGeometryStreamer::stats() const
{
    std::lock_guard lock(impl_->mutex);
    return impl_->stats;
}

} // namespace Halcyon::Renderer::Scene
