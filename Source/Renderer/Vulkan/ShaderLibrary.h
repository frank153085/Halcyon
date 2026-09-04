#pragma once

#include "Core/Result.h"
#include "VulkanShaderModule.h"

#include <string_view>
#include <vulkan/vulkan.h>

namespace Halcyon::Vulkan
{

// A small policy layer over VulkanShaderModule. Shader binaries are always
// loaded from the configured DXC output directory; missing binaries are fatal.
class ShaderLibrary final
{
public:
    ShaderLibrary() noexcept = default;
    explicit ShaderLibrary(VkDevice device) noexcept
            : moduleLoader_(device)
    {
    }

    void reset(VkDevice device) noexcept;

    [[nodiscard]] Halcyon::Result<VkShaderModule> create(std::string_view fileName,
        Halcyon::Renderer::Shaders::ShaderReflection* reflection = nullptr);
    [[nodiscard]] Halcyon::Result<Halcyon::Renderer::Shaders::ShaderReflection> reflect(
        std::string_view fileName) const;
    [[nodiscard]] Halcyon::Result<bool> reload(VkShaderModule& current,
        std::string_view fileName,
        Halcyon::Renderer::Shaders::ShaderReflection* reflection = nullptr);

    void destroy(VkShaderModule& module) noexcept;

private:
    VulkanShaderModule moduleLoader_;
};

} // namespace Halcyon::Vulkan
