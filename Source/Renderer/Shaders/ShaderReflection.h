#pragma once

#include "Core/Result.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Halcyon::Renderer::Shaders
{

enum class ResourceType : std::uint8_t
{
    Unknown,
    SampledImage,
    StorageImage,
    Sampler,
    UniformBuffer,
    StorageBuffer,
};

struct ResourceBinding
{
    std::uint32_t set = 0;
    std::uint32_t binding = 0;
    std::uint32_t arraySize = 1;
    ResourceType type = ResourceType::Unknown;
    std::uint32_t variableId = 0;
};

struct PushConstantRange
{
    std::uint32_t variableId = 0;
    std::uint32_t size = 0;
};

struct ShaderReflection
{
    std::vector<ResourceBinding> resources;
    std::vector<PushConstantRange> pushConstants;
    std::vector<std::uint32_t> outputLocations;
};

// Reflects the descriptor and push-constant subset needed by Halcyon's
// pipeline layout builder.  The parser intentionally stays small and
// dependency-free; SPIR-V Tools remains an optional validation pass.
[[nodiscard]] Halcyon::Result<ShaderReflection> reflectSpirv(std::span<const std::uint32_t> words);

} // namespace Halcyon::Renderer::Shaders
