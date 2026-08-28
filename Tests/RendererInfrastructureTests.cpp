#include "Renderer/Resources/DeferredDeletionQueue.hpp"
#include "Renderer/Resources/UploadRing.hpp"

#ifndef HALCYON_BUILD_EXPERIMENTAL_M2
#    define HALCYON_BUILD_EXPERIMENTAL_M2 0
#endif

#if HALCYON_BUILD_EXPERIMENTAL_M2
#    include "Renderer/Graph/BarrierPlanner.hpp"
#    include "Renderer/Resources/BindlessTable.hpp"
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>

namespace
{

class TestContext
{
public:
    void expect(bool condition, std::string_view expression, int line)
    {
        if (!condition)
        {
            ++failures_;
            std::cerr << "FAILED line " << line << ": " << expression << '\n';
        }
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

private:
    int failures_ = 0;
};

#define HALCYON_EXPECT(context, expression) \
    (context).expect(static_cast<bool>(expression), #expression, __LINE__)

void uploadRingTests(TestContext& context)
{
    using Halcyon::Renderer::Resources::UploadRing;
    UploadRing ring(32);
    ring.beginFrame(20);
    const std::array<std::uint32_t, 2> values{0x12345678u, 0x9abcdef0u};
    const auto first = ring.write(std::span<const std::uint32_t>{values});
    HALCYON_EXPECT(context, first);
    HALCYON_EXPECT(context, first.value().offset % alignof(std::uint32_t) == 0u);
    HALCYON_EXPECT(context, ring.frameUploadedBytes() == values.size() * sizeof(values[0]));
    const auto overBudget = ring.allocate(16, 4);
    HALCYON_EXPECT(context, !overBudget);
    HALCYON_EXPECT(context, ring.pendingAllocations() == 1u);
    ring.collect(0);
    HALCYON_EXPECT(context, ring.pendingAllocations() == 0u);
    ring.collect(1);
    // A second collect remains harmless after the allocation has retired.
    HALCYON_EXPECT(context, ring.pendingAllocations() == 0u);

    ring.beginFrame();
    const auto wrapped = ring.allocate(24, 8, 7);
    HALCYON_EXPECT(context, wrapped);
    HALCYON_EXPECT(context, ring.freeBytes() < ring.capacity());
    ring.collect(6);
    HALCYON_EXPECT(context, ring.pendingAllocations() == 1u);
    ring.collect(7);
    HALCYON_EXPECT(context, ring.pendingAllocations() == 0u);
}

void deletionQueueTests(TestContext& context)
{
    Halcyon::Renderer::Resources::DeferredDeletionQueue queue;
    int destroyed = 0;
    queue.enqueue(5, [&destroyed] { ++destroyed; });
    queue.enqueue(2, [&destroyed] { destroyed += 10; });
    HALCYON_EXPECT(context, queue.collect(1) == 0u);
    HALCYON_EXPECT(context, queue.collect(2) == 1u);
    HALCYON_EXPECT(context, destroyed == 10);
    HALCYON_EXPECT(context, queue.flush() == 1u);
    HALCYON_EXPECT(context, destroyed == 11);
}

#if HALCYON_BUILD_EXPERIMENTAL_M2
void barrierPlannerTests(TestContext& context)
{
    namespace Graph = Halcyon::Renderer::Graph;
    const auto texture = Graph::TextureHandle{3u, 1u};
    const std::array<Graph::ResourceAccess, 1> write{
        Graph::ResourceAccess{Graph::ResourceKind::Texture, texture.index(),
                              texture.generation(), Graph::AccessMode::Write,
                              Graph::ResourceUsage::ColorAttachment}};
    const std::array<Graph::ResourceAccess, 1> read{
        Graph::ResourceAccess{Graph::ResourceKind::Texture, texture.index(),
                              texture.generation(), Graph::AccessMode::Read,
                              Graph::ResourceUsage::Sampled}};
    Graph::BarrierPlanner planner;
    const auto first = planner.plan(write, Graph::QueueClass::Graphics);
    HALCYON_EXPECT(context, first.size() == 1u && first.front().required);
    const auto second = planner.plan(read, Graph::QueueClass::Compute);
    HALCYON_EXPECT(context, second.size() == 1u && second.front().required);
    HALCYON_EXPECT(context, second.front().queueOwnershipTransfer);
    HALCYON_EXPECT(context, second.front().before.layout == Graph::ImageLayout::ColorAttachment);
    HALCYON_EXPECT(context, second.front().after.layout == Graph::ImageLayout::ShaderReadOnly);
    HALCYON_EXPECT(context, planner.state(Graph::ResourceKind::Texture, 3u, 1u) != nullptr);
    planner.begin();
    HALCYON_EXPECT(context, planner.trackedResourceCount() == 0u);
}

void bindlessTests(TestContext& context)
{
    using namespace Halcyon::Renderer::Resources;
    BindlessTableConfig config;
    config.sampledImageCapacity = 2;
    config.storageImageCapacity = 2;
    config.uniformBufferCapacity = 1;
    config.storageBufferCapacity = 1;
    config.samplerCapacity = 1;
    BindlessTable table;
    HALCYON_EXPECT(context, table.initialize(config));
    const auto defaultHandle = table.defaultHandle(DescriptorType::SampledImage);
    HALCYON_EXPECT(context, defaultHandle.valid() && defaultHandle.index() == 0u);
    const auto first = table.allocateSampledImage(DescriptorValue{42u, 7u, 1u});
    HALCYON_EXPECT(context, first && first.value().index() == 1u);
    const auto exhausted = table.allocateSampledImage();
    HALCYON_EXPECT(context, !exhausted);
    HALCYON_EXPECT(context, table.touch(DescriptorType::SampledImage, first.value(), 9u));
    HALCYON_EXPECT(context, table.release(DescriptorType::SampledImage, first.value(), 3u));
    HALCYON_EXPECT(context, table.pending(DescriptorType::SampledImage, first.value()));
    HALCYON_EXPECT(context, table.collect(8u) == 0u);
    HALCYON_EXPECT(context, table.collect(9u) == 1u);
    const auto replacement = table.allocateSampledImage({}, 9u);
    HALCYON_EXPECT(context, replacement && replacement.value().index() == 1u);
    HALCYON_EXPECT(context, replacement.value().generation() != first.value().generation());
    HALCYON_EXPECT(context, !table.contains(DescriptorType::StorageImage, replacement.value()));
    table.shutdown();
    HALCYON_EXPECT(context, !table.initialized());
    HALCYON_EXPECT(context, table.initialize(config));

    // The table exposes one completion timeline shared by all descriptor
    // types.  Advancing it through one type must also make slots retired by
    // another type reusable, even if the later allocate call carries an
    // older completion value.
    const auto storage = table.allocateStorageImage();
    HALCYON_EXPECT(context, storage);
    HALCYON_EXPECT(context,
                   table.release(DescriptorType::StorageImage, storage.value(), 5u));
    const auto timelineAdvance = table.allocateSampledImage({}, 10u);
    HALCYON_EXPECT(context, timelineAdvance);
    const auto recycledStorage = table.allocateStorageImage({}, 0u);
    HALCYON_EXPECT(context, recycledStorage);
    HALCYON_EXPECT(context, recycledStorage.value().index() == storage.value().index());
    HALCYON_EXPECT(context,
                   recycledStorage.value().generation() != storage.value().generation());
}
#endif

} // namespace

int main()
{
    TestContext context;
    uploadRingTests(context);
    deletionQueueTests(context);
#if HALCYON_BUILD_EXPERIMENTAL_M2
    barrierPlannerTests(context);
    bindlessTests(context);
#endif
    if (context.failures() != 0)
    {
        std::cerr << context.failures() << " infrastructure test(s) failed\n";
        return 1;
    }
    std::cout << "All renderer infrastructure tests passed\n";
    return 0;
}
