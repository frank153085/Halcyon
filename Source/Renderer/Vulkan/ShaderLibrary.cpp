#include "ShaderLibrary.h"

namespace Halcyon::Vulkan
{

void ShaderLibrary::reset(VkDevice device) noexcept
{
    moduleLoader_.reset(device);
}

Halcyon::Result<VkShaderModule> ShaderLibrary::create(
    std::string_view fileName, std::span<const std::uint32_t> fallback)
{
    return moduleLoader_.create(fileName, fallback);
}

void ShaderLibrary::destroy(VkShaderModule& module) noexcept
{
    moduleLoader_.destroy(module);
}

} // namespace Halcyon::Vulkan
