#pragma once

#include "Core/Result.h"

#include <string>
#include <vulkan/vulkan.h>

namespace Halcyon::Vulkan
{

using VoidResult = Halcyon::Result<void>;

[[nodiscard]] VoidResult ok() noexcept;
[[nodiscard]] VoidResult fail(
    std::string message, Halcyon::ErrorCode code = Halcyon::ErrorCode::Backend);
[[nodiscard]] const char* resultName(VkResult result) noexcept;
[[nodiscard]] std::string vkFailure(const char* operation, VkResult result);

} // namespace Halcyon::Vulkan
