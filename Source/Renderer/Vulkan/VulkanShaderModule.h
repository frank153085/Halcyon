#pragma once

#include "Core/Result.h"
#include "Renderer/Shaders/ShaderReflection.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.h>

namespace Halcyon::Vulkan
{

class VulkanShaderModule final
{
public:
    VulkanShaderModule() noexcept = default;
    explicit VulkanShaderModule(VkDevice device) noexcept
            : device_(device)
    {
    }

    void reset(VkDevice device) noexcept
    {
        device_ = device;
    }

    [[nodiscard]] Halcyon::Result<VkShaderModule> create(std::string_view fileName,
        Halcyon::Renderer::Shaders::ShaderReflection* reflection = nullptr);
    // Parse descriptor and push-constant metadata without creating a Vulkan
    // object. This is useful for pipeline-layout planning and CPU tests.
    [[nodiscard]] Halcyon::Result<Halcyon::Renderer::Shaders::ShaderReflection> reflect(
        std::string_view fileName) const;
    // Build a replacement module first and only swap it into current after a
    // successful load/validation.  A failed reload therefore leaves the last
    // valid module untouched, which is the key safety property for shader
    // hot-reload during development.
    [[nodiscard]] Halcyon::Result<bool> reload(VkShaderModule& current,
        std::string_view fileName,
        Halcyon::Renderer::Shaders::ShaderReflection* reflection = nullptr);
    void destroy(VkShaderModule& module) noexcept;

private:
    [[nodiscard]] std::vector<std::uint32_t> load(std::string_view fileName) const;

    VkDevice device_ = VK_NULL_HANDLE;
};

} // namespace Halcyon::Vulkan
