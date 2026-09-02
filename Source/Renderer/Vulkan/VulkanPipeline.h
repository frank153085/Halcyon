#pragma once

#include "Core/Result.h"
#include "GpuResourceManager.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace Halcyon::Vulkan
{

struct alignas(16) TexturedPushConstants
{
    glm::mat4 viewProjection{1.0f};
    glm::mat4 model{1.0f};
};
static_assert(sizeof(TexturedPushConstants) == sizeof(glm::mat4) * 2);

class VulkanPipeline final
{
public:
    VulkanPipeline() noexcept = default;
    ~VulkanPipeline() noexcept
    {
        destroy();
    }
    VulkanPipeline(const VulkanPipeline&) = delete;
    VulkanPipeline& operator=(const VulkanPipeline&) = delete;

    [[nodiscard]] Halcyon::Result<void> create(VkDevice device,
        VkFormat colorFormat,
        VkFormat depthFormat,
        VkExtent2D extent,
        VkDescriptorSetLayout textureSetLayout,
        VkDescriptorSetLayout bindlessSetLayout,
        bool texturedRequested);
    void destroy() noexcept;

    [[nodiscard]] VkPipeline pipeline() const noexcept
    {
        return pipeline_;
    }
    [[nodiscard]] VkPipelineLayout layout() const noexcept
    {
        return layout_;
    }
    [[nodiscard]] bool textured() const noexcept
    {
        return textured_;
    }
    [[nodiscard]] bool bindless() const noexcept
    {
        return bindless_;
    }

private:
    [[nodiscard]] Halcyon::Result<void> createInternal(VkDevice device,
        VkFormat colorFormat,
        VkFormat depthFormat,
        VkExtent2D extent,
        VkDescriptorSetLayout textureSetLayout,
        VkDescriptorSetLayout bindlessSetLayout,
        bool texturedRequested);
    void swap(VulkanPipeline& other) noexcept;

    VkDevice device_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkShaderModule vertexShader_ = VK_NULL_HANDLE;
    VkShaderModule fragmentShader_ = VK_NULL_HANDLE;
    bool textured_ = false;
    bool bindless_ = false;
};

} // namespace Halcyon::Vulkan
