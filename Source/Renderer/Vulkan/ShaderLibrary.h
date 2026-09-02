#pragma once

#include "Core/Result.h"
#include "VulkanShaderModule.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vulkan/vulkan.h>

namespace Halcyon::Vulkan
{

// A small policy layer over VulkanShaderModule.  Keeping file lookup and the
// embedded fallback in one library gives pipeline code a stable shader-facing
// interface and leaves room for hot-reload/cache policies in later milestones.
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
        std::span<const std::uint32_t> fallback = {},
        Halcyon::Renderer::Shaders::ShaderReflection* reflection = nullptr);
    [[nodiscard]] Halcyon::Result<Halcyon::Renderer::Shaders::ShaderReflection> reflect(
        std::string_view fileName, std::span<const std::uint32_t> fallback = {}) const;
    [[nodiscard]] Halcyon::Result<bool> reload(VkShaderModule& current,
        std::string_view fileName,
        std::span<const std::uint32_t> fallback = {},
        Halcyon::Renderer::Shaders::ShaderReflection* reflection = nullptr);

    void destroy(VkShaderModule& module) noexcept;

private:
    VulkanShaderModule moduleLoader_;
};

} // namespace Halcyon::Vulkan
