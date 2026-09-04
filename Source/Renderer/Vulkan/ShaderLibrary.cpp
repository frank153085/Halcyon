#include "ShaderLibrary.h"

namespace Halcyon::Vulkan
{

void ShaderLibrary::reset(VkDevice device) noexcept
{
    moduleLoader_.reset(device);
}

Halcyon::Result<VkShaderModule> ShaderLibrary::create(std::string_view fileName,
    Halcyon::Renderer::Shaders::ShaderReflection* reflection)
{
    return moduleLoader_.create(fileName, reflection);
}

Halcyon::Result<Halcyon::Renderer::Shaders::ShaderReflection> ShaderLibrary::reflect(
    std::string_view fileName) const
{
    return moduleLoader_.reflect(fileName);
}

Halcyon::Result<bool> ShaderLibrary::reload(VkShaderModule& current,
    std::string_view fileName,
    Halcyon::Renderer::Shaders::ShaderReflection* reflection)
{
    return moduleLoader_.reload(current, fileName, reflection);
}

void ShaderLibrary::destroy(VkShaderModule& module) noexcept
{
    moduleLoader_.destroy(module);
}

} // namespace Halcyon::Vulkan
