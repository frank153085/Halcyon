#include "PipelineRegistry.h"

namespace Halcyon::Vulkan
{

void PipelineRegistry::destroySwapchainResources(VkDevice device) noexcept
{
    csmDepthPipeline.destroy();
    gbufferPipeline.destroy();
    gbufferDoubleSidedPipeline.destroy();
    deferredLightingPipeline.destroy();
    transparentPipeline.destroy();
    transparentDoubleSidedPipeline.destroy();
    taaPipeline.destroy();
    tonemapPipeline.destroy();
    clusterBuildPipeline.destroy();
    frustumCullPipeline.destroy();
    indirectBuildPipeline.destroy();
    gpuDrivenGbufferPipeline.destroy();
    hizBuildPipeline.destroy();
    occlusionPhase1Pipeline.destroy();
    occlusionPhase2Pipeline.destroy();
    if (device != VK_NULL_HANDLE)
    {
        if (gpuSceneDescriptorPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(device, gpuSceneDescriptorPool, nullptr);
        }
        if (gpuSceneCullLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(device, gpuSceneCullLayout, nullptr);
        }
        if (gpuSceneIndirectLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(device, gpuSceneIndirectLayout, nullptr);
        }
        if (gpuSceneGraphicsLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(device, gpuSceneGraphicsLayout, nullptr);
        }
        if (hizLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(device, hizLayout, nullptr);
        }
        if (occlusionPhase1Layout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(device, occlusionPhase1Layout, nullptr);
        }
        if (occlusionPhase2Layout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(device, occlusionPhase2Layout, nullptr);
        }
    }
    gpuSceneDescriptorPool = VK_NULL_HANDLE;
    gpuSceneCullLayout = VK_NULL_HANDLE;
    gpuSceneIndirectLayout = VK_NULL_HANDLE;
    gpuSceneGraphicsLayout = VK_NULL_HANDLE;
    hizLayout = VK_NULL_HANDLE;
    occlusionPhase1Layout = VK_NULL_HANDLE;
    occlusionPhase2Layout = VK_NULL_HANDLE;
}

void PipelineRegistry::destroyM3Layouts(VkDevice device) noexcept
{
    if (device != VK_NULL_HANDLE)
    {
        if (m3MaterialLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(device, m3MaterialLayout, nullptr);
        }
        if (m3LightingLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(device, m3LightingLayout, nullptr);
        }
        if (m3TaaLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(device, m3TaaLayout, nullptr);
        }
        if (m3ClusterLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(device, m3ClusterLayout, nullptr);
        }
        if (m3TonemapLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(device, m3TonemapLayout, nullptr);
        }
    }
    m3MaterialLayout = VK_NULL_HANDLE;
    m3LightingLayout = VK_NULL_HANDLE;
    m3TaaLayout = VK_NULL_HANDLE;
    m3ClusterLayout = VK_NULL_HANDLE;
    m3TonemapLayout = VK_NULL_HANDLE;
}

} // namespace Halcyon::Vulkan
