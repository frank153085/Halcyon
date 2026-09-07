#pragma once

#include "VulkanPipeline.h"

#include <vulkan/vulkan.h>

namespace Halcyon::Vulkan
{

// Owns graphics/compute pipelines and the descriptor layouts they consume.
// Renderer::Impl remains the composition root and asks this object to create
// and destroy backend pipeline state.
class PipelineRegistry final
{
public:
    PipelineRegistry() = default;
    PipelineRegistry(const PipelineRegistry&) = delete;
    PipelineRegistry& operator=(const PipelineRegistry&) = delete;

    void destroySwapchainResources(VkDevice device) noexcept;
    void destroyFrameLayouts(VkDevice device) noexcept;

    VulkanPipeline csmDepthPipeline;
    VulkanPipeline gbufferPipeline;
    VulkanPipeline gbufferDoubleSidedPipeline;
    VulkanPipeline deferredLightingPipeline;
    VulkanPipeline transparentPipeline;
    VulkanPipeline transparentDoubleSidedPipeline;
    VulkanPipeline taaPipeline;
    VulkanPipeline tonemapPipeline;
    VulkanPipeline clusterBuildPipeline;
    VulkanPipeline frustumCullPipeline;
    VulkanPipeline indirectBuildPipeline;
    VulkanPipeline gpuDrivenGbufferPipeline;
    VulkanPipeline gpuDrivenCsmPipeline;
    VulkanPipeline hizBuildPipeline;
    VulkanPipeline occlusionPhase1Pipeline;
    VulkanPipeline occlusionPhase2Pipeline;

    VkDescriptorSetLayout gpuSceneCullLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout gpuSceneIndirectLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout gpuSceneGraphicsLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout gpuCsmGraphicsLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout hizLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout occlusionPhase1Layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout occlusionPhase2Layout = VK_NULL_HANDLE;
    VkDescriptorPool gpuSceneDescriptorPool = VK_NULL_HANDLE;

    VkDescriptorSetLayout materialLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout lightingLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout taaLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout clusterLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout tonemapLayout = VK_NULL_HANDLE;
};

} // namespace Halcyon::Vulkan
