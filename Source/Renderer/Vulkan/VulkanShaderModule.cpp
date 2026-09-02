#include "VulkanShaderModule.h"

#include "EmbeddedTriangleShaders.h"
#include "VulkanCommon.h"

#include <cstring>
#include <fstream>
#include <string>

#if defined(HALCYON_ENABLE_SPIRV_TOOLS)
#include <spirv-tools/libspirv.hpp>
#endif

namespace Halcyon::Vulkan
{

std::vector<std::uint32_t> VulkanShaderModule::load(std::string_view fileName) const
{
#ifdef HALCYON_SHADER_DIR
    const std::string path = std::string(HALCYON_SHADER_DIR) + "/" + std::string(fileName);
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (file)
    {
        const auto size = file.tellg();
        if (size > 0 && (size % static_cast<std::streamoff>(sizeof(std::uint32_t))) == 0)
        {
            std::vector<std::uint32_t> code(static_cast<std::size_t>(size) / sizeof(std::uint32_t));
            file.seekg(0);
            file.read(reinterpret_cast<char*>(code.data()), size);
            if (file)
            {
                return code;
            }
        }
    }
#else
    (void)fileName;
#endif
    return {};
}

Halcyon::Result<VkShaderModule> VulkanShaderModule::create(std::string_view fileName,
    std::span<const std::uint32_t> fallback,
    Halcyon::Renderer::Shaders::ShaderReflection* reflection)
{
    if (device_ == VK_NULL_HANDLE)
    {
        return Halcyon::Result<VkShaderModule>::failure(
            {Halcyon::ErrorCode::InvalidState, "shader module device is not initialized"});
    }
    std::vector<std::uint32_t> fileCode = load(fileName);
    const std::span<const std::uint32_t> code =
        fileCode.empty() ? fallback : std::span<const std::uint32_t>{fileCode};
    if (code.empty())
    {
        return Halcyon::Result<VkShaderModule>::failure(
            {Halcyon::ErrorCode::NotFound, "SPIR-V shader binary is unavailable"});
    }
    if (code.size() < 5u || code.front() != 0x07230203u)
    {
        return Halcyon::Result<VkShaderModule>::failure(
            {Halcyon::ErrorCode::InvalidArgument, "shader binary is not a valid SPIR-V module"});
    }
#if defined(HALCYON_ENABLE_SPIRV_TOOLS)
    spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_3);
    if (!validator.Validate(code.data(), code.size()))
    {
        return Halcyon::Result<VkShaderModule>::failure(
            {Halcyon::ErrorCode::InvalidArgument, "SPIR-V validation failed"});
    }
#endif
    const auto reflectionResult = Halcyon::Renderer::Shaders::reflectSpirv(code);
    if (!reflectionResult)
    {
        return Halcyon::Result<VkShaderModule>::failure(reflectionResult.error());
    }
    if (reflection != nullptr)
    {
        *reflection = reflectionResult.value();
    }
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size_bytes();
    info.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    const VkResult result = vkCreateShaderModule(device_, &info, nullptr, &module);
    if (result != VK_SUCCESS)
    {
        return Halcyon::Result<VkShaderModule>::failure(
            {Halcyon::ErrorCode::Backend, vkFailure("vkCreateShaderModule", result)});
    }
    return Halcyon::Result<VkShaderModule>::success(module);
}

Halcyon::Result<Halcyon::Renderer::Shaders::ShaderReflection> VulkanShaderModule::reflect(
    std::string_view fileName, std::span<const std::uint32_t> fallback) const
{
    const std::vector<std::uint32_t> fileCode = load(fileName);
    const std::span<const std::uint32_t> code =
        fileCode.empty() ? fallback : std::span<const std::uint32_t>{fileCode};
    if (code.empty())
    {
        return Halcyon::Result<Halcyon::Renderer::Shaders::ShaderReflection>::failure(
            {Halcyon::ErrorCode::NotFound, "SPIR-V shader binary is unavailable"});
    }
    return Halcyon::Renderer::Shaders::reflectSpirv(code);
}

Halcyon::Result<bool> VulkanShaderModule::reload(VkShaderModule& current,
    std::string_view fileName,
    std::span<const std::uint32_t> fallback,
    Halcyon::Renderer::Shaders::ShaderReflection* reflection)
{
    const auto replacement = create(fileName, fallback, reflection);
    if (!replacement)
    {
        return Halcyon::Result<bool>::failure(replacement.error());
    }
    VkShaderModule old = current;
    current = replacement.value();
    destroy(old);
    return Halcyon::Result<bool>::success(true);
}

void VulkanShaderModule::destroy(VkShaderModule& module) noexcept
{
    if (device_ != VK_NULL_HANDLE && module != VK_NULL_HANDLE)
    {
        vkDestroyShaderModule(device_, module, nullptr);
    }
    module = VK_NULL_HANDLE;
}

} // namespace Halcyon::Vulkan
