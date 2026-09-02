#include "Renderer/Shaders/ShaderReflection.h"

#include <array>
#include <cstdint>
#include <iostream>
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

    [[nodiscard]] int failures() const noexcept
    {
        return failures_;
    }

private:
    int failures_ = 0;
};

#define HALCYON_EXPECT(context, expression)                                                        \
    (context).expect(static_cast<bool>(expression), #expression, __LINE__)

constexpr std::uint32_t instruction(std::uint16_t wordCount, std::uint16_t opcode) noexcept
{
    return (static_cast<std::uint32_t>(wordCount) << 16u) | opcode;
}

void invalidModuleTests(TestContext& context)
{
    constexpr std::array<std::uint32_t, 5> invalid = {0u, 0x00010300u, 0u, 16u, 0u};
    const auto result = Halcyon::Renderer::Shaders::reflectSpirv(invalid);
    HALCYON_EXPECT(context, !result);
    HALCYON_EXPECT(context, result.error().code == Halcyon::ErrorCode::InvalidArgument);

    constexpr std::array<std::uint32_t, 6> truncated = {
        0x07230203u, 0x00010300u, 0u, 16u, 0u, instruction(4, 59)};
    const auto truncatedResult = Halcyon::Renderer::Shaders::reflectSpirv(truncated);
    HALCYON_EXPECT(context, !truncatedResult);
}

void descriptorAndPushConstantTests(TestContext& context)
{
    using Halcyon::Renderer::Shaders::ResourceType;

    // This compact module declares a sampled image at set 1/binding 2 and a
    // 16-byte push constant block. It is intentionally assembled by hand so
    // the test remains independent of a shader compiler.
    constexpr std::array<std::uint32_t, 63> module = {
        0x07230203u,
        0x00010300u,
        0u,
        32u,
        0u,
        instruction(3, 22),
        1u,
        32u, // OpTypeFloat %1
        instruction(9, 25),
        2u,
        1u,
        1u,
        0u,
        0u,
        0u,
        1u,
        0u, // OpTypeImage %2
        instruction(2, 26),
        5u, // OpTypeSampler %5
        instruction(4, 32),
        3u,
        0u,
        2u, // OpTypePointer UniformConstant %2
        instruction(4, 32),
        6u,
        0u,
        5u, // OpTypePointer UniformConstant %5
        instruction(4, 59),
        3u,
        4u,
        0u, // OpVariable %4
        instruction(4, 59),
        6u,
        7u,
        0u, // OpVariable %7
        instruction(4, 71),
        4u,
        34u,
        1u, // OpDecorate %4 DescriptorSet 1
        instruction(4, 71),
        4u,
        33u,
        2u, // OpDecorate %4 Binding 2
        instruction(4, 23),
        8u,
        1u,
        4u, // OpTypeVector %8
        instruction(3, 30),
        9u,
        8u, // OpTypeStruct %9
        instruction(4, 32),
        10u,
        9u,
        9u, // OpTypePointer PushConstant %9
        instruction(4, 59),
        10u,
        11u,
        9u, // OpVariable %11
        instruction(5, 72),
        9u,
        0u,
        35u,
        0u, // OpMemberDecorate %9 offset 0
    };

    const auto result = Halcyon::Renderer::Shaders::reflectSpirv(module);
    HALCYON_EXPECT(context, result);
    if (!result)
    {
        std::cerr << result.error().describe() << '\n';
        return;
    }
    const auto& reflection = result.value();
    HALCYON_EXPECT(context, reflection.resources.size() == 1u);
    if (!reflection.resources.empty())
    {
        const auto& resource = reflection.resources.front();
        HALCYON_EXPECT(context, resource.set == 1u);
        HALCYON_EXPECT(context, resource.binding == 2u);
        HALCYON_EXPECT(context, resource.type == ResourceType::SampledImage);
        HALCYON_EXPECT(context, resource.variableId == 4u);
    }
    HALCYON_EXPECT(context, reflection.pushConstants.size() == 1u);
    if (!reflection.pushConstants.empty())
    {
        HALCYON_EXPECT(context, reflection.pushConstants.front().variableId == 11u);
        HALCYON_EXPECT(context, reflection.pushConstants.front().size == 16u);
    }
}

} // namespace

int main()
{
    TestContext context;
    invalidModuleTests(context);
    descriptorAndPushConstantTests(context);

    if (context.failures() != 0)
    {
        std::cerr << context.failures() << " shader-reflection test(s) failed\n";
        return 1;
    }
    std::cout << "All shader-reflection tests passed\n";
    return 0;
}
