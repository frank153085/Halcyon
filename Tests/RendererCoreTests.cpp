#include "Renderer/Graph/RenderGraph.h"
#include "Renderer/Quality/FrameBudgetController.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{

class TestContext
{
public:
    void expect(bool condition, std::string_view expression, int line)
    {
        if (condition)
        {
            return;
        }
        ++failures_;
        std::cerr << "FAILED line " << line << ": " << expression << '\n';
    }

    [[nodiscard]] int failures() const noexcept
    {
        return failures_;
    }

private:
    int failures_ = 0;
};

#define HALCYON_EXPECT(context, expression)                                                        \
    (context).expect(static_cast<bool>(expression), #expression, __LINE__)

void renderGraphDependencyAndCullingTests(TestContext& context)
{
    namespace Graph = Halcyon::Renderer::Graph;

    Graph::RenderGraph graph;
    const auto visibility = graph.createTexture({.name = "Visibility"});
    const auto hdr = graph.createTexture({.name = "HDR"});
    const auto unused = graph.createTexture({.name = "Unused"});

    auto visibilityPass = graph.addPass("Visibility");
    visibilityPass.write(visibility, Graph::ResourceUsage::ColorAttachment);

    auto shadingPass = graph.addPass("Compute shading");
    shadingPass.read(visibility, Graph::ResourceUsage::Sampled)
        .write(hdr, Graph::ResourceUsage::Storage);

    auto presentPass = graph.addPass("Present");
    presentPass.read(hdr, Graph::ResourceUsage::Sampled).setSideEffect();

    auto unusedPass = graph.addPass("Unused pass");
    unusedPass.write(unused, Graph::ResourceUsage::Storage);

    const Graph::CompileResult compiled = graph.compile();
    HALCYON_EXPECT(context, compiled);
    HALCYON_EXPECT(context, compiled.executionOrder.size() == 3);
    HALCYON_EXPECT(context, compiled.executionOrder[0] == visibilityPass.handle());
    HALCYON_EXPECT(context, compiled.executionOrder[1] == shadingPass.handle());
    HALCYON_EXPECT(context, compiled.executionOrder[2] == presentPass.handle());
    HALCYON_EXPECT(context, compiled.isCulled(unusedPass.handle()));

    const auto* visibilityLifetime = compiled.lifetime(visibility);
    const auto* hdrLifetime = compiled.lifetime(hdr);
    HALCYON_EXPECT(context, visibilityLifetime != nullptr);
    HALCYON_EXPECT(context, hdrLifetime != nullptr);
    HALCYON_EXPECT(context, visibilityLifetime->firstUse == 0);
    HALCYON_EXPECT(context, visibilityLifetime->lastUse == 1);
    HALCYON_EXPECT(context, hdrLifetime->firstUse == 1);
    HALCYON_EXPECT(context, hdrLifetime->lastUse == 2);

    std::vector<std::string> execution;
    auto executableGraph = Graph::RenderGraph{};
    const auto output = executableGraph.createTexture({.name = "Output"});
    auto producer = executableGraph.addPass("Producer");
    producer.write(output, Graph::ResourceUsage::Storage)
        .setExecute(
            [&execution](const Graph::PassExecutionContext& pass)
            {
                execution.emplace_back(pass.name);
            });
    auto consumer = executableGraph.addPass("Consumer");
    consumer.read(output, Graph::ResourceUsage::Sampled)
        .setSideEffect()
        .setExecute(
            [&execution](const Graph::PassExecutionContext& pass)
            {
                execution.emplace_back(pass.name);
            });
    const auto executable = executableGraph.compile();
    const auto executionResult = executable.execute();
    HALCYON_EXPECT(context, executionResult);
    HALCYON_EXPECT(context, executionResult.executedPasses == 2u);
    HALCYON_EXPECT(context, execution.size() == 2u);
    HALCYON_EXPECT(context, execution[0] == "Producer");
    HALCYON_EXPECT(context, execution[1] == "Consumer");

    std::vector<std::string> boundaries;
    std::vector<std::uint32_t> indices;
    bool callbackUserDataValid = false;
    Graph::ExecuteOptions executeOptions;
    executeOptions.userData = &boundaries;
    executeOptions.onBegin = [&boundaries, &indices, &callbackUserDataValid](
                                 const Graph::PassExecutionContext& pass)
    {
        boundaries.emplace_back("begin:" + std::string(pass.name));
        indices.push_back(pass.executionIndex);
        callbackUserDataValid = callbackUserDataValid || pass.userData != nullptr;
    };
    executeOptions.onEnd = [&boundaries](const Graph::PassExecutionContext& pass)
    {
        boundaries.emplace_back("end:" + std::string(pass.name));
    };
    const auto boundaryResult = executable.execute(executeOptions);
    HALCYON_EXPECT(context, boundaryResult);
    HALCYON_EXPECT(context, boundaries.size() == 4u);
    HALCYON_EXPECT(context, boundaries[0] == "begin:Producer");
    HALCYON_EXPECT(context, boundaries[1] == "end:Producer");
    HALCYON_EXPECT(context, boundaries[2] == "begin:Consumer");
    HALCYON_EXPECT(context, boundaries[3] == "end:Consumer");
    HALCYON_EXPECT(context, indices.size() == 2u && indices[0] == 0u && indices[1] == 1u);
    HALCYON_EXPECT(context, callbackUserDataValid);

    auto failingGraph = Graph::RenderGraph{};
    auto failingPass = failingGraph.addPass("Failing", true);
    failingPass.setExecute(
        [](const Graph::PassExecutionContext&)
        {
            throw std::runtime_error("test callback failure");
        });
    const auto failedExecution = failingGraph.compile().execute();
    HALCYON_EXPECT(context, !failedExecution);
    HALCYON_EXPECT(context, failedExecution.error.code == Graph::GraphErrorCode::ExecutionFailed);
}

void renderGraphCycleAndGenerationTests(TestContext& context)
{
    namespace Graph = Halcyon::Renderer::Graph;

    Graph::RenderGraph graph;
    const auto oldBuffer = graph.createBuffer({.name = "Old", .size = 64});
    HALCYON_EXPECT(context, graph.valid(oldBuffer));
    HALCYON_EXPECT(context, graph.destroy(oldBuffer));
    HALCYON_EXPECT(context, !graph.valid(oldBuffer));
    const auto replacement = graph.createBuffer({.name = "Replacement", .size = 64});
    HALCYON_EXPECT(context, replacement.index() == oldBuffer.index());
    HALCYON_EXPECT(context, replacement.generation() != oldBuffer.generation());

    auto first = graph.addPass("First", true);
    auto second = graph.addPass("Second", true);
    first.dependsOn(second.handle());
    second.dependsOn(first.handle());

    const Graph::CompileResult compiled = graph.compile();
    HALCYON_EXPECT(context, !compiled);
    HALCYON_EXPECT(context, compiled.error.code == Graph::GraphErrorCode::CycleDetected);
    HALCYON_EXPECT(context, !compiled.error.cycle.empty());
}

void frameBudgetDowngradeAndUpgradeTests(TestContext& context)
{
    namespace Quality = Halcyon::Renderer::Quality;

    Quality::FrameBudgetConfig config;
    config.initialQuality = Quality::QualityState::highestQuality();
    Quality::FrameBudgetController controller(config);
    const auto initial = controller.quality();

    Quality::FrameBudgetUpdate update;
    for (std::uint32_t frame = 1; frame <= config.downgradeAfterFrames; ++frame)
    {
        update = controller.update(frame, 30.0);
    }

    HALCYON_EXPECT(context, update.adjusted);
    HALCYON_EXPECT(context, update.decision.has_value());
    HALCYON_EXPECT(context, update.decision->direction == Quality::AdjustmentDirection::Downgrade);
    HALCYON_EXPECT(context, controller.quality() != initial);
    HALCYON_EXPECT(context, controller.decisionLog().size() == 1);

    bool upgraded = false;
    const std::uint64_t firstUnderBudgetFrame = config.downgradeAfterFrames + 1u;
    for (std::uint64_t offset = 0; offset < config.upgradeAfterFrames + 8u; ++offset)
    {
        const auto underBudget = controller.update(firstUnderBudgetFrame + offset, 1.0);
        if (underBudget.adjusted)
        {
            HALCYON_EXPECT(context, underBudget.decision.has_value());
            HALCYON_EXPECT(
                context, underBudget.decision->direction == Quality::AdjustmentDirection::Upgrade);
            upgraded = true;
            break;
        }
    }
    HALCYON_EXPECT(context, upgraded);
    HALCYON_EXPECT(context, controller.decisionLog().size() == 2);
}

} // namespace

int main()
{
    TestContext context;
    renderGraphDependencyAndCullingTests(context);
    renderGraphCycleAndGenerationTests(context);
    frameBudgetDowngradeAndUpgradeTests(context);

    if (context.failures() != 0)
    {
        std::cerr << context.failures() << " renderer-core test(s) failed\n";
        return 1;
    }

    std::cout << "All renderer-core tests passed\n";
    return 0;
}
