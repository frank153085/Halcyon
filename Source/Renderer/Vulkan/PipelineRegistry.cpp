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

void PipelineRegistry::destroyFrameLayouts(VkDevice device) noexcept
{
    if (device != VK_NULL_HANDLE)
    {
        if (materialLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(device, materialLayout, nullptr);
        }
        if (lightingLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(device, lightingLayout, nullptr);
        }
        if (taaLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(device, taaLayout, nullptr);
        }
        if (clusterLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(device, clusterLayout, nullptr);
        }
        if (tonemapLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(device, tonemapLayout, nullptr);
        }
    }
    materialLayout = VK_NULL_HANDLE;
    lightingLayout = VK_NULL_HANDLE;
    taaLayout = VK_NULL_HANDLE;
    clusterLayout = VK_NULL_HANDLE;
    tonemapLayout = VK_NULL_HANDLE;
}

} // namespace Halcyon::Vulkan
