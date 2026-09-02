#include "Renderer/Graph/FrameGraph.h"
#include "Renderer/Quality/FrameBudgetController.h"

#include <gtest/gtest.h>

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

TEST(M2FrameGraph, ExecutesDependenciesInTopologicalOrder)
{
    using namespace Halcyon::Renderer::Graph;
    FrameGraph graph;
    const auto output = graph.createTexture({.name = "Output"});
    auto producer = graph.addPass("Producer");
    producer.write(output, ResourceUsage::Storage);
    auto consumer = graph.addPass("Consumer");
    consumer.read(output, ResourceUsage::Sampled).setSideEffect();

    const auto compiled = graph.compile();
    ASSERT_TRUE(compiled);
    ASSERT_EQ(compiled.executionOrder.size(), 2u);
    EXPECT_EQ(compiled.executionOrder[0], producer.handle());
    EXPECT_EQ(compiled.executionOrder[1], consumer.handle());
}

TEST(M2FrameBudget, DowngradesAfterSustainedOverBudget)
{
    using namespace Halcyon::Renderer::Quality;
    FrameBudgetController controller;
    const auto initial = controller.quality();
    FrameBudgetUpdate update;
    for (std::uint64_t frame = 1; frame <= 8; ++frame)
    {
        update = controller.update(frame, 30.0);
    }

    EXPECT_TRUE(update.adjusted);
    ASSERT_TRUE(update.decision.has_value());
    EXPECT_EQ(update.decision->direction, AdjustmentDirection::Downgrade);
    EXPECT_NE(controller.quality(), initial);
}
