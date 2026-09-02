#pragma once

#include "Core/Result.h"

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

    [[nodiscard]] Halcyon::Result<VkShaderModule> create(
        std::string_view fileName, std::span<const std::uint32_t> fallback = {});
    void destroy(VkShaderModule& module) noexcept;

private:
    [[nodiscard]] std::vector<std::uint32_t> load(
        std::string_view fileName) const;

    VkDevice device_ = VK_NULL_HANDLE;
};

} // namespace Halcyon::Vulkan
