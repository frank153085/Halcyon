#include "Renderer/Resources/DeferredDeletionQueue.hpp"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace
{

using Queue = Halcyon::Renderer::Resources::DeferredDeletionQueue;

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

void orderingAndTimelineTest(TestContext& context)
{
    std::vector<int> order;
    Queue queue;

    queue.enqueue(10, [&order] { order.push_back(10); });
    queue.enqueue(3, [&order] { order.push_back(3); });
    queue.enqueue(3, [&order] { order.push_back(4); });

    HALCYON_EXPECT(context, queue.size() == 3);
    const auto collectedBeforeReady = queue.collect(2);
    HALCYON_EXPECT(context, collectedBeforeReady == 0);
    HALCYON_EXPECT(context, queue.size() == 3);
    const auto collectedAtThree = queue.collect(3);
    HALCYON_EXPECT(context, collectedAtThree == 2);
    HALCYON_EXPECT(context, (order == std::vector<int>{3, 4}));
    const auto flushed = queue.flush();
    HALCYON_EXPECT(context, flushed == 1);
    HALCYON_EXPECT(context, (order == std::vector<int>{3, 4, 10}));
    HALCYON_EXPECT(context, queue.empty());
}

void moveOnlyAndExceptionTest(TestContext& context)
{
    int destroyed = 0;
    Queue queue;

    queue.enqueue(1, [owner = std::make_unique<int>(7), &destroyed]() mutable {
        destroyed += *owner;
    });
    queue.enqueue(1, [] { throw std::runtime_error("first callback failure"); });
    queue.enqueue(1, [] { throw std::runtime_error("last callback failure"); });

    const auto collected = queue.collect(1);
    HALCYON_EXPECT(context, collected == 3);
    HALCYON_EXPECT(context, destroyed == 7);
    HALCYON_EXPECT(context, queue.empty());
    HALCYON_EXPECT(context, queue.errorCount() == 2);
    const auto lastException = queue.lastException();
    HALCYON_EXPECT(context, lastException != nullptr);
    bool caughtRuntimeError = false;
    if (lastException != nullptr)
    {
        try
        {
            std::rethrow_exception(lastException);
        }
        catch (const std::runtime_error& error)
        {
            caughtRuntimeError = true;
            HALCYON_EXPECT(context,
                           std::string_view{error.what()} == "last callback failure");
        }
        catch (...)
        {
        }
    }
    HALCYON_EXPECT(context, caughtRuntimeError);

    queue.clearErrors();
    HALCYON_EXPECT(context, !queue.hasErrors());
}

} // namespace

int main()
{
    TestContext context;
    orderingAndTimelineTest(context);
    moveOnlyAndExceptionTest(context);
    if (context.failures() != 0)
    {
        std::cerr << context.failures() << " deferred-deletion test(s) failed\n";
        return 1;
    }
    std::cout << "All deferred-deletion tests passed\n";
    return 0;
}
