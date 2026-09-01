#include "Core/Core.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

struct TextureTag;
using TextureHandle = Halcyon::Handle<TextureTag>;

struct Texture
{
    std::string name;
};

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

void resultTests(TestContext& context)
{
    auto value = Halcyon::Result<int>::success(42);
    HALCYON_EXPECT(context, value);
    HALCYON_EXPECT(context, value.value() == 42);

    auto failure = Halcyon::Result<int>::failure(
        Halcyon::MakeError(Halcyon::ErrorCode::InvalidState, "broken", "test"));
    HALCYON_EXPECT(context, !failure);
    HALCYON_EXPECT(context, failure.error().code == Halcyon::ErrorCode::InvalidState);
    HALCYON_EXPECT(context, failure.error().describe().find("broken") != std::string::npos);

    auto voidSuccess = Halcyon::Result<void>::success();
    HALCYON_EXPECT(context, voidSuccess);
    auto voidFailure = Halcyon::Result<void>::failure(
        Halcyon::MakeError(Halcyon::ErrorCode::Unsupported, "feature"));
    HALCYON_EXPECT(context, !voidFailure);
}

void handlePoolTests(TestContext& context)
{
    Halcyon::HandlePool<Texture, TextureHandle> pool;
    const auto defaultResult = pool.emplaceDefault(Texture{"fallback"});
    HALCYON_EXPECT(context, defaultResult);
    const TextureHandle fallback = defaultResult.value();
    HALCYON_EXPECT(context, fallback.index() == 0);
    HALCYON_EXPECT(context, pool.defaultHandle() == fallback);
    HALCYON_EXPECT(context, !pool.erase(fallback));

    const TextureHandle first = pool.emplace(Texture{"first"});
    HALCYON_EXPECT(context, first.index() != 0);
    HALCYON_EXPECT(context, pool.contains(first));
    HALCYON_EXPECT(context, pool.get(first)->name == "first");
    HALCYON_EXPECT(context, pool.erase(first));
    HALCYON_EXPECT(context, !pool.contains(first));

    const TextureHandle replacement = pool.emplace(Texture{"replacement"});
    HALCYON_EXPECT(context, replacement.index() == first.index());
    HALCYON_EXPECT(context, replacement.generation() != first.generation());
    HALCYON_EXPECT(context, pool.get(first) == nullptr);
    HALCYON_EXPECT(context, pool.get(replacement)->name == "replacement");

    pool.clear();
    HALCYON_EXPECT(context, !pool.contains(replacement));
    HALCYON_EXPECT(context, !pool.defaultHandle());
    const auto restored = pool.emplaceDefault(Texture{"restored fallback"});
    HALCYON_EXPECT(context, restored);
    HALCYON_EXPECT(context, restored.value().index() == 0);

    // A moved-from pool remains a valid empty object.  In particular its
    // free-list head must not retain an index into the moved-away vector.
    Halcyon::HandlePool<Texture, TextureHandle> source;
    const TextureHandle movedHandle = source.emplace(Texture{"moved"});
    Halcyon::HandlePool<Texture, TextureHandle> destination{std::move(source)};
    HALCYON_EXPECT(context, destination.contains(movedHandle));
    const TextureHandle reusedSource = source.emplace(Texture{"source reused"});
    HALCYON_EXPECT(context, source.contains(reusedSource));
    HALCYON_EXPECT(context, source.get(reusedSource)->name == "source reused");
}

void loggerTests(TestContext& context)
{
    std::vector<std::string> messages;
    auto& logger = Halcyon::Logger::instance();
    logger.setLevel(Halcyon::LogLevel::Info);
    logger.setSink(
        [&messages](Halcyon::LogLevel, std::string_view message)
        {
            messages.emplace_back(message);
        });
    logger.debug("hidden");
    logger.info("visible ", 7);
    logger.resetSink();

    HALCYON_EXPECT(context, messages.size() == 1);
    HALCYON_EXPECT(context, messages.front() == "visible 7");
}

} // namespace

int main()
{
    TestContext context;
    resultTests(context);
    handlePoolTests(context);
    loggerTests(context);

    if (context.failures() != 0)
    {
        std::cerr << context.failures() << " core test(s) failed\n";
        return 1;
    }

    std::cout << "All core tests passed\n";
    return 0;
}
