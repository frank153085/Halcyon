#pragma once

#include "Core/Result.h"
#include "GpuResourceManager.h"

#include <glm/glm.hpp>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.h>

namespace Halcyon::Vulkan
{

struct DescriptorBindingDesc
{
    std::uint32_t set = 0;
    VkDescriptorSetLayoutBinding binding{};
};

struct alignas(16) TexturedPushConstants
{
    glm::mat4 viewProjection{1.0f};
    glm::mat4 model{1.0f};
};

struct GraphicsPipelineDesc
{
    std::span<const VkFormat> colorFormats{};
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    bool depthOnly = false;
    bool depthTest = true;
    bool depthWrite = true;
    VkCompareOp depthCompare = VK_COMPARE_OP_GREATER_OR_EQUAL;
    VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
    bool depthBiasEnable = false;
    float depthBiasConstant = 0.0f;
    float depthBiasSlope = 0.0f;
    bool blendEnable = false;
    std::span<const VkDescriptorSetLayout> descriptorLayouts{};
    std::span<const DescriptorBindingDesc> descriptorBindings{};
    std::span<const VkPushConstantRange> pushConstants{};
    std::string_view vertexShader{};
    std::string_view fragmentShader{};
};

struct ComputePipelineDesc
{
    std::string_view shader{};
    std::span<const VkDescriptorSetLayout> descriptorLayouts{};
    std::span<const DescriptorBindingDesc> descriptorBindings{};
    std::span<const VkPushConstantRange> pushConstants{};
};
static_assert(sizeof(TexturedPushConstants) == sizeof(glm::mat4) * 2);

struct alignas(16) M3PushConstants
{
    glm::mat4 viewProjection{1.0f};
    glm::mat4 previousViewProjection{1.0f};
    glm::mat4 model{1.0f};
    glm::mat4 previousModel{1.0f};
};
static_assert(sizeof(M3PushConstants) == 256);

struct alignas(16) M3VertexPushConstants
{
    glm::mat4 viewProjection{1.0f};
    glm::mat4 previousViewProjection{1.0f};
    glm::mat4 model{1.0f};
    glm::mat4 previousModel{1.0f};
};
static_assert(sizeof(M3VertexPushConstants) == 256);

struct alignas(16) M3TransparentPushConstants
{
    glm::mat4 viewProjection{1.0f};
    glm::vec4 cameraPosition{0.0f, 0.0f, 0.0f, 1.0f};
    glm::vec4 lightPositionOrDirection{0.0f, -1.0f, 0.0f, 1.0f};
    glm::vec4 lightColorIntensity{1.0f};
    glm::vec4 lightParameters{10.0f, 0.9f, 0.8f, 0.04f};
    glm::mat4 model{1.0f};
    glm::mat4 unusedPreviousModel{0.0f};
};
static_assert(sizeof(M3TransparentPushConstants) == 256);

struct alignas(16) M3MaterialPushConstants
{
    glm::vec4 baseColorFactor{1.0f};
    glm::vec4 emissiveFactor{0.0f};
    glm::vec4 factors{0.0f, 1.0f, 0.5f, 0.0f}; // metallic, roughness, alpha cutoff, flags
};
static_assert(sizeof(M3MaterialPushConstants) == 48);

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

    [[nodiscard]] Halcyon::Result<void> createGraphics(
        VkDevice device, const GraphicsPipelineDesc& desc);
    [[nodiscard]] Halcyon::Result<void> createCompute(
        VkDevice device, const ComputePipelineDesc& desc);
    void destroy() noexcept;

    [[nodiscard]] VkPipeline pipeline() const noexcept
    {
        return pipeline_;
    }
    [[nodiscard]] VkPipelineLayout layout() const noexcept
    {
        return layout_;
    }
    [[nodiscard]] bool compute() const noexcept { return computePipeline_ != VK_NULL_HANDLE; }
    [[nodiscard]] VkPipeline computePipeline() const noexcept { return computePipeline_; }

private:
    [[nodiscard]] Halcyon::Result<void> createGraphicsInternal(
        VkDevice device, const GraphicsPipelineDesc& desc);
    [[nodiscard]] Halcyon::Result<void> createComputeInternal(
        VkDevice device, const ComputePipelineDesc& desc);
    void swap(VulkanPipeline& other) noexcept;

    VkDevice device_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline computePipeline_ = VK_NULL_HANDLE;
    VkShaderModule vertexShader_ = VK_NULL_HANDLE;
    VkShaderModule fragmentShader_ = VK_NULL_HANDLE;
    VkShaderModule computeShader_ = VK_NULL_HANDLE;
};

} // namespace Halcyon::Vulkan
