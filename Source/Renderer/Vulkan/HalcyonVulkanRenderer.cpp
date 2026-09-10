#include "HalcyonVulkanRenderer.h"

#include "Core/Profiler.h"
#include "Core/Log.h"
#include "DebugReadbackManager.h"
#include "FrameRecorder.h"
#include "FramePassContext.h"
#include "GpuAllocator.h"
#include "GpuUploader.h"
#include "IndirectBuildDescriptorLayout.h"
#include "PipelineRegistry.h"
#include "RendererNativeHandles.h"
#include "VulkanBindlessTable.h"
#include "VulkanGpuSceneBuffers.h"
#include "VulkanCommon.h"
#include "VulkanDevice.h"
#include "VulkanFrameContext.h"
#include "VulkanFrameGraphProvider.h"
#include "VulkanFrameResources.h"
#include "VulkanPipeline.h"
#include "VulkanSceneResources.h"
#include "VirtualGeometryPass.h"
#include "VulkanSwapchain.h"
#include "../Graph/FrameGraph.h"
#include "../Graph/BarrierPlanner.h"
#include "../Scene/Ecs/RenderableManager.h"
#include "../Scene/GpuScene.h"

#ifndef HALCYON_BUILD_FRAMEGRAPH
#define HALCYON_BUILD_FRAMEGRAPH 0
#endif

#if HALCYON_BUILD_FRAMEGRAPH
#include "../Graph/BarrierPlanner.h"
#include "../Graph/FrameGraph.h"
#endif

// GLFW is included here (rather than in the public header) so applications
// can choose their own GLFW include policy.  The Vulkan include guard makes
// this safe when the caller included glfw3.h with GLFW_INCLUDE_VULKAN first.
#ifndef GLFW_INCLUDE_VULKAN
#define GLFW_INCLUDE_VULKAN
#endif
#include <GLFW/glfw3.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#undef STB_IMAGE_WRITE_IMPLEMENTATION
#include <filesystem>
#include <fstream>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <new>
#include <limits>
#include <utility>
#include <vector>

namespace Halcyon::Vulkan
{
namespace Graph = Halcyon::Renderer::Graph;
#if HALCYON_BUILD_FRAMEGRAPH
#endif
namespace
{

// The shader uses a fixed-size descriptor array because Vulkan pipeline
// layouts must agree with the statically reflected array length. Devices that
// cannot expose this complete table intentionally use the per-material
// fallback below.
constexpr std::uint32_t kGpuDrivenSampledImageCapacity = 256u;
constexpr std::uint32_t kGpuDrivenSamplerCapacity = 16u;

#if HALCYON_BUILD_FRAMEGRAPH
[[nodiscard]] std::uint32_t descriptorCapacity(
    std::uint32_t preferred, std::uint32_t perStageLimit, std::uint32_t setLimit) noexcept
{
    return std::max(1u, std::min({preferred, perStageLimit, setLimit}));
}

[[nodiscard]] Resources::BindlessTableConfig bindlessConfig(
    const VkPhysicalDeviceLimits& limits) noexcept
{
    Resources::BindlessTableConfig config;
    config.sampledImageCapacity = descriptorCapacity(
        kGpuDrivenSampledImageCapacity, limits.maxPerStageDescriptorSampledImages,
        limits.maxDescriptorSetSampledImages);
    config.storageImageCapacity = descriptorCapacity(
        8u, limits.maxPerStageDescriptorStorageImages, limits.maxDescriptorSetStorageImages);
    config.uniformBufferCapacity = descriptorCapacity(
        12u, limits.maxPerStageDescriptorUniformBuffers, limits.maxDescriptorSetUniformBuffers);
    config.storageBufferCapacity = descriptorCapacity(
        8u, limits.maxPerStageDescriptorStorageBuffers, limits.maxDescriptorSetStorageBuffers);
    config.samplerCapacity = descriptorCapacity(
        kGpuDrivenSamplerCapacity, limits.maxPerStageDescriptorSamplers,
        limits.maxDescriptorSetSamplers);
    return config;
}

[[nodiscard]] VkPipelineStageFlags2 toVkStages(Graph::PipelineStage stages) noexcept
{
    VkPipelineStageFlags2 result = VK_PIPELINE_STAGE_2_NONE;
    if (Graph::any(stages & Graph::PipelineStage::VertexInput))
    {
        result |= VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
    }
    if (Graph::any(stages & Graph::PipelineStage::VertexShader))
    {
        result |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    }
    if (Graph::any(stages & Graph::PipelineStage::FragmentShader))
    {
        result |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    }
    if (Graph::any(stages & Graph::PipelineStage::ComputeShader))
    {
        result |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    }
    if (Graph::any(stages & Graph::PipelineStage::ColorOutput))
    {
        result |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    if (Graph::any(stages & Graph::PipelineStage::DepthTest))
    {
        result |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                  VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    }
    if (Graph::any(stages & Graph::PipelineStage::Transfer))
    {
        result |= VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    }
    if (Graph::any(stages & Graph::PipelineStage::DrawIndirect))
    {
        result |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    }
    if (Graph::any(stages & Graph::PipelineStage::Host))
    {
        result |= VK_PIPELINE_STAGE_2_HOST_BIT;
    }
    if (Graph::any(stages & Graph::PipelineStage::AllCommands))
    {
        result |= VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }
    return result;
}

[[nodiscard]] VkAccessFlags2 toVkAccess(Graph::AccessFlags access) noexcept
{
    VkAccessFlags2 result = VK_ACCESS_2_NONE;
    if (Graph::any(access & Graph::AccessFlags::VertexRead))
    {
        result |= VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
    }
    if (Graph::any(access & Graph::AccessFlags::IndexRead))
    {
        result |= VK_ACCESS_2_INDEX_READ_BIT;
    }
    if (Graph::any(access & Graph::AccessFlags::UniformRead))
    {
        result |= VK_ACCESS_2_UNIFORM_READ_BIT;
    }
    if (Graph::any(access & Graph::AccessFlags::ShaderSampledRead))
    {
        result |= VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    }
    if (Graph::any(access & Graph::AccessFlags::ShaderStorageRead))
    {
        result |= VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    }
    if (Graph::any(access & Graph::AccessFlags::ShaderStorageWrite))
    {
        result |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    }
    if (Graph::any(access & Graph::AccessFlags::IndirectRead))
    {
        result |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    }
    if (Graph::any(access & Graph::AccessFlags::ColorWrite))
    {
        result |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    }
    if (Graph::any(access & Graph::AccessFlags::DepthRead))
    {
        result |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    }
    if (Graph::any(access & Graph::AccessFlags::DepthWrite))
    {
        result |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }
    if (Graph::any(access & Graph::AccessFlags::TransferRead))
    {
        result |= VK_ACCESS_2_TRANSFER_READ_BIT;
    }
    if (Graph::any(access & Graph::AccessFlags::TransferWrite))
    {
        result |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
    }
    if (Graph::any(access & Graph::AccessFlags::HostWrite))
    {
        result |= VK_ACCESS_2_HOST_WRITE_BIT;
    }
    if (Graph::any(access & Graph::AccessFlags::ColorRead))
    {
        result |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
    }
    if (Graph::any(access & Graph::AccessFlags::PresentRead))
    {
        result |= VK_ACCESS_2_NONE;
    }
    return result;
}

[[nodiscard]] VkImageLayout toVkLayout(Graph::ImageLayout layout) noexcept
{
    switch (layout)
    {
        case Graph::ImageLayout::Undefined:
            return VK_IMAGE_LAYOUT_UNDEFINED;
        case Graph::ImageLayout::General:
            return VK_IMAGE_LAYOUT_GENERAL;
        case Graph::ImageLayout::ShaderReadOnly:
            return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case Graph::ImageLayout::ColorAttachment:
            return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case Graph::ImageLayout::DepthAttachment:
            return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        case Graph::ImageLayout::TransferSource:
            return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case Graph::ImageLayout::TransferDestination:
            return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        case Graph::ImageLayout::Present:
            return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }
    return VK_IMAGE_LAYOUT_GENERAL;
}
#endif

} // namespace

struct Renderer::Impl
{
    static constexpr std::uint32_t VisibilityReadbackCapacity =
        DebugReadbackManager::VisibilityReadbackCapacity;
    // frustum candidates, phase-1 visible, phase-2 visible, phase-1 command
    // count, and phase-2 command count.
    static constexpr std::uint32_t VisibilityReadbackHeaderCount =
        DebugReadbackManager::VisibilityReadbackHeaderCount;
    using FrameContext = VulkanFrame;

    RendererConfig config{};
    GLFWwindow* window = nullptr;
    std::string lastError;

    VulkanDevice deviceState;
    VkInstance& instance = deviceState.instance;
    VkDebugUtilsMessengerEXT& debugMessenger = deviceState.debugMessenger;
    VkSurfaceKHR& surface = deviceState.surface;
    VkPhysicalDevice& physicalDevice = deviceState.physicalDevice;
    VkPhysicalDeviceProperties& physicalProperties = deviceState.physicalProperties;
    VkPhysicalDeviceMemoryProperties& memoryProperties = deviceState.memoryProperties;
    VkDevice& device = deviceState.device;
    VkQueue& graphicsQueue = deviceState.graphicsQueue;
    VkQueue& presentQueue = deviceState.presentQueue;
    PFN_vkCmdDrawMeshTasksIndirectCountEXT& cmdDrawMeshTasksIndirectCount =
        deviceState.cmdDrawMeshTasksIndirectCount;
    std::uint32_t& graphicsQueueFamily = deviceState.graphicsQueueFamily;
    std::uint32_t& presentQueueFamily = deviceState.presentQueueFamily;
    Capabilities& caps = deviceState.capabilities;
    std::uint64_t& deviceLocalBytes = deviceState.deviceLocalBytes;

    // Swapchain, depth target and presentation semaphores are owned by the
    // dedicated swapchain module.  References keep the frame code concise
    // while retaining a single source of truth for these handles.
    VulkanSwapchain swapchainState;
    VkSwapchainKHR& swapchain = swapchainState.swapchain;
    VkFormat& swapchainFormat = swapchainState.swapchainFormat;
    VkColorSpaceKHR& swapchainColorSpace = swapchainState.swapchainColorSpace;
    VkExtent2D& swapchainExtent = swapchainState.swapchainExtent;
    VkFormat& depthFormat = swapchainState.depthFormat;
    std::vector<VkImage>& swapchainImages = swapchainState.swapchainImages;
    std::vector<VkImageView>& swapchainImageViews = swapchainState.swapchainImageViews;
    std::vector<VkSemaphore>& presentReadySemaphores = swapchainState.presentReadySemaphores;
    std::vector<bool>& swapchainImageInitialized = swapchainState.swapchainImageInitialized;

    VulkanFrameContext frameContext;
    std::vector<FrameContext>& frames = frameContext.frames;
    std::uint32_t& currentFrame = frameContext.currentFrame;
    bool& timestampsEnabled = frameContext.timestampsEnabled;

    PipelineRegistry pipelines;
    FrameRecorder frameRecorder;
    bool gpuDrivenBindless = false;
    VulkanPipeline& csmDepthPipeline = pipelines.csmDepthPipeline;
    VulkanPipeline& gbufferPipeline = pipelines.gbufferPipeline;
    VulkanPipeline& gbufferDoubleSidedPipeline = pipelines.gbufferDoubleSidedPipeline;
    VulkanPipeline& deferredLightingPipeline = pipelines.deferredLightingPipeline;
    VulkanPipeline& transparentPipeline = pipelines.transparentPipeline;
    VulkanPipeline& transparentDoubleSidedPipeline = pipelines.transparentDoubleSidedPipeline;
    VulkanPipeline& taaPipeline = pipelines.taaPipeline;
    VulkanPipeline& tonemapPipeline = pipelines.tonemapPipeline;
    VulkanPipeline& clusterBuildPipeline = pipelines.clusterBuildPipeline;
    VulkanPipeline& frustumCullPipeline = pipelines.frustumCullPipeline;
    VulkanPipeline& indirectBuildPipeline = pipelines.indirectBuildPipeline;
    VulkanPipeline& gpuDrivenGbufferPipeline = pipelines.gpuDrivenGbufferPipeline;
    VulkanPipeline& gpuDrivenGbufferDoubleSidedPipeline = pipelines.gpuDrivenGbufferDoubleSidedPipeline;
    VulkanPipeline& gpuDrivenCsmPipeline = pipelines.gpuDrivenCsmPipeline;
    VulkanPipeline& hizBuildPipeline = pipelines.hizBuildPipeline;
    VulkanPipeline& occlusionPhase1Pipeline = pipelines.occlusionPhase1Pipeline;
    VulkanPipeline& occlusionPhase2Pipeline = pipelines.occlusionPhase2Pipeline;
    VulkanPipeline& meshletCullPipeline = pipelines.meshletCullPipeline;
    VulkanPipeline& meshletIndirectPipeline = pipelines.meshletIndirectPipeline;
    VulkanPipeline& meshletMeshIndirectPipeline = pipelines.meshletMeshIndirectPipeline;
    VulkanPipeline& virtualGeometryMeshPipeline = pipelines.virtualGeometryMeshPipeline;
    VulkanPipeline& lodSelectPipeline = pipelines.lodSelectPipeline;
    VulkanPipeline& visibilityPipeline = pipelines.visibilityPipeline;
    VulkanPipeline& materialClassifyPipeline = pipelines.materialClassifyPipeline;
    VulkanPipeline& computeShadingPipeline = pipelines.computeShadingPipeline;
    VkDescriptorSetLayout& gpuSceneCullLayout = pipelines.gpuSceneCullLayout;
    VkDescriptorSetLayout& gpuSceneIndirectLayout = pipelines.gpuSceneIndirectLayout;
    VkDescriptorSetLayout& gpuSceneGraphicsLayout = pipelines.gpuSceneGraphicsLayout;
    VkDescriptorSetLayout& gpuCsmGraphicsLayout = pipelines.gpuCsmGraphicsLayout;
    VkDescriptorSetLayout& hizLayout = pipelines.hizLayout;
    VkDescriptorSetLayout& occlusionPhase1Layout = pipelines.occlusionPhase1Layout;
    VkDescriptorSetLayout& occlusionPhase2Layout = pipelines.occlusionPhase2Layout;
    VkDescriptorSetLayout& meshletCullLayout = pipelines.meshletCullLayout;
    VkDescriptorSetLayout& meshletIndirectLayout = pipelines.meshletIndirectLayout;
    VkDescriptorSetLayout& meshletMeshIndirectLayout = pipelines.meshletMeshIndirectLayout;
    VkDescriptorSetLayout& virtualGeometryMeshLayout = pipelines.virtualGeometryMeshLayout;
    VkDescriptorSetLayout& lodSelectLayout = pipelines.lodSelectLayout;
    VkDescriptorSetLayout& visibilityLayout = pipelines.visibilityLayout;
    VkDescriptorSetLayout& materialClassifyLayout = pipelines.materialClassifyLayout;
    VkDescriptorSetLayout& computeShadingLayout = pipelines.computeShadingLayout;
    VkDescriptorPool& gpuSceneDescriptorPool = pipelines.gpuSceneDescriptorPool;
    VkDescriptorSetLayout& materialLayout = pipelines.materialLayout;
    VkDescriptorSetLayout& lightingLayout = pipelines.lightingLayout;
    VkDescriptorSetLayout& taaLayout = pipelines.taaLayout;
    VkDescriptorSetLayout& clusterLayout = pipelines.clusterLayout;
    VkDescriptorSetLayout& tonemapLayout = pipelines.tonemapLayout;
    VkDescriptorPool frameDescriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> frameDescriptorPools;
    VkSampler linearSampler = VK_NULL_HANDLE;

    VkExtent2D& requestedExtent = swapchainState.requestedExtent;
    bool& framebufferResized = swapchainState.framebufferResized;
    bool initialized = false;
    bool deviceLost = false;
    bool fatalError = false;
    bool& rayQueryEnabled = deviceState.rayQueryEnabled;
    VkDeviceSize deviceMemoryBytes = 0;
    GpuAllocator gpuAllocator;
    GpuUploader gpuUploader;
    DebugReadbackManager debugReadbacks;
    std::vector<BufferAllocation>& clusterOverflowReadbacks = debugReadbacks.clusterOverflowReadbacks;
    std::vector<BufferAllocation>& gpuVisibilityReadbacks = debugReadbacks.gpuVisibilityReadbacks;
    std::vector<bool>& gpuVisibilityValid = debugReadbacks.gpuVisibilityValid;
    std::vector<BufferAllocation>& virtualGeometryReadbacks = debugReadbacks.virtualGeometryReadbacks;
    std::vector<bool>& virtualGeometryValid = debugReadbacks.virtualGeometryValid;
    std::vector<BufferAllocation>& virtualPageRequestReadbacks =
        debugReadbacks.virtualPageRequestReadbacks;
    std::vector<bool>& virtualPageRequestValid = debugReadbacks.virtualPageRequestValid;
    std::vector<std::uint64_t>& virtualPageRequestFrameIndices =
        debugReadbacks.virtualPageRequestFrameIndices;
    std::vector<std::uint32_t>& virtualPageRequestPageCounts =
        debugReadbacks.virtualPageRequestPageCounts;
    std::vector<std::uint32_t>& virtualPageRequestMeshIds =
        debugReadbacks.virtualPageRequestMeshIds;
    std::vector<std::vector<std::uint32_t>>& gpuReferenceVisible = debugReadbacks.gpuReferenceVisible;
    std::vector<BufferAllocation>& instanceIdReadbacks = debugReadbacks.instanceIdReadbacks;
    std::vector<bool>& instanceIdReadbackValid = debugReadbacks.instanceIdReadbackValid;
    std::vector<std::uint64_t>& instanceIdReadbackFrameIndices =
        debugReadbacks.instanceIdReadbackFrameIndices;
    std::vector<std::vector<BufferAllocation>> frameUploadBuffers;
    VulkanFrameGraphProvider frameGraphProvider;
    VulkanFrameResources frameResources;
    VulkanSceneResources sceneResources;
    VulkanGpuSceneBuffers gpuSceneBuffers;
    VulkanBindlessTable bindlessTable;
    OverlayCallback overlayCallback = nullptr;
    bool taaHistoryValid = false;
    bool taaHistoryFlip = false;
    bool taaHistoryInitializedA = false;
    bool taaHistoryInitializedB = false;
    bool iblInitialized = false;
    bool iblImageInitialized = false;
    bool virtualHiZInitialized = false;
    bool virtualHiZImageInitialized = false;
    bool virtualVisibilityValid = false;
    bool hasRenderedFrame = false;
    std::uint64_t lastFrameIndex = 0;
    std::uint64_t renderSerial = 0;
    std::vector<InstanceData> previousInstances;
    glm::mat4 previousViewProjection{1.0f};
    bool previousPacketValid = false;
    std::filesystem::path& pendingScreenshotPath = debugReadbacks.pendingScreenshotPath;
    std::uint64_t gpuSceneContentHash = 0;
    std::uint32_t gpuSceneInstanceCount = 0;
    bool gpuDrivenActive = false;
    // The configured path may be downgraded for an individual frame when a
    // packet or transient resource is incompatible. Keep statistics tied to
    // the path actually recorded into the command buffer.
    bool virtualGeometryActive = false;
    std::string meshShaderFallbackReason;
    Halcyon::Renderer::Scene::RenderPathMode activeRenderPath =
        Halcyon::Renderer::Scene::RenderPathMode::DeferredIndexed;
    std::uint32_t gpuFallbackInstanceCount = 0;
    std::uint32_t gpuMaterialCount = 0;
    std::uint32_t materialDescriptorBindCount = 0;
    std::vector<Resources::DescriptorHandle> bindlessTextureHandles;
    Resources::DescriptorHandle bindlessSamplerHandle{};
    std::vector<Halcyon::Renderer::Scene::MaterialGpuData> bindlessMaterialRows;
    Halcyon::Renderer::Scene::GpuSceneState gpuSceneState{131072};

    ~Impl()
    {
        cleanup();
    }

    void setError(std::string message)
    {
        lastError = std::move(message);
    }

    [[nodiscard]] VoidResult synchronizeBindlessMaterials()
    {
        const auto sampledType = Resources::DescriptorType::SampledImage;
        const bool hasBindlessTextures = bindlessTable.initialized();
        if (hasBindlessTextures)
        {
            bindlessTextureHandles.resize(sceneResources.textureCount());
            for (std::uint32_t dense = 0; dense < sceneResources.textureCount(); ++dense)
            {
                const TextureResource* texture = sceneResources.textureDense(dense);
                if (texture == nullptr) continue;
                auto& handle = bindlessTextureHandles[dense];
                if (!handle.valid())
                {
                    const auto allocated = bindlessTable.allocate(sampledType);
                    if (!allocated) return fail(allocated.error().describe());
                    handle = allocated.value();
                }
                const VkDescriptorImageInfo image{VK_NULL_HANDLE, texture->view,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                const auto write = bindlessTable.writeImage(sampledType, handle, image);
                if (!write) return fail(write.error().describe());
            }
            if (const TextureResource* texture = sceneResources.textureDense(0); texture != nullptr)
            {
                const VkDescriptorImageInfo image{VK_NULL_HANDLE, texture->view,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                const auto write = bindlessTable.writeImage(sampledType,
                    bindlessTable.table().defaultHandle(sampledType), image);
                if (!write) return fail(write.error().describe());
                const VkDescriptorImageInfo sampler{texture->sampler, VK_NULL_HANDLE,
                    VK_IMAGE_LAYOUT_UNDEFINED};
                const auto samplerWrite = bindlessTable.writeImage(Resources::DescriptorType::Sampler,
                    bindlessTable.table().defaultHandle(Resources::DescriptorType::Sampler), sampler);
                if (!samplerWrite) return fail(samplerWrite.error().describe());
            }
        }
        bindlessMaterialRows.resize(sceneResources.materialCount());
        for (std::uint32_t dense = 0; dense < sceneResources.materialCount(); ++dense)
        {
            auto row = sceneResources.materialRow(dense);
            if (hasBindlessTextures)
            {
                for (std::size_t texture = 0; texture < 5; ++texture)
                {
                    const auto denseTexture = row.textureIndices[texture];
                    row.textureIndices[texture] = denseTexture < bindlessTextureHandles.size() &&
                            bindlessTextureHandles[denseTexture].valid()
                        ? bindlessTextureHandles[denseTexture].index()
                        : bindlessTable.table().defaultHandle(sampledType).index();
                }
            }
            bindlessMaterialRows[dense] = row;
        }
        gpuMaterialCount = static_cast<std::uint32_t>(bindlessMaterialRows.size());
        if (!bindlessMaterialRows.empty())
        {
            const auto upload = gpuSceneBuffers.uploadMaterials(bindlessMaterialRows);
            if (!upload) return fail(upload.error().describe());
        }
        return ok();
    }

    void cleanupSwapchain() noexcept
    {
        gpuDrivenBindless = false;
        pipelines.destroySwapchainResources(device);
        frameResources.reset();
        frameGraphProvider.recreatePersistent();
        swapchainState.cleanup();
    }

    void cleanupFrameDescriptors() noexcept
    {
        if (device != VK_NULL_HANDLE)
        {
            for (auto pool : frameDescriptorPools)
                if (pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, pool, nullptr);
            if (linearSampler != VK_NULL_HANDLE)
                vkDestroySampler(device, linearSampler, nullptr);
        }
        pipelines.destroyFrameLayouts(device);
        frameDescriptorPool = VK_NULL_HANDLE;
        frameDescriptorPools.clear();
        linearSampler = VK_NULL_HANDLE;
    }

    void cleanup() noexcept
    {
        if (device != VK_NULL_HANDLE)
        {
            // Waiting is best effort during error cleanup; every child object
            // is still destroyed even when the device has already been lost.
            (void)vkDeviceWaitIdle(device);
            cleanupSwapchain();
            cleanupFrameDescriptors();
            frameGraphProvider.shutdown();
            sceneResources.cleanup();
            gpuSceneBuffers.cleanup();
            bindlessTable.shutdown();
            debugReadbacks.cleanup(gpuAllocator);
            for (auto& frameUploads : frameUploadBuffers)
                for (auto& upload : frameUploads)
                    gpuAllocator.destroy(upload);
            frameUploadBuffers.clear();
            frameContext.cleanup(device);
            gpuAllocator.shutdown();
        }
        deviceState.cleanup();
        initialized = false;
        deviceLost = false;
        fatalError = false;
        deviceMemoryBytes = 0;
        timestampsEnabled = false;
        currentFrame = 0;
        framebufferResized = false;
        requestedExtent = {};
        window = nullptr;
        overlayCallback = nullptr;
        taaHistoryValid = false;
        taaHistoryFlip = false;
        taaHistoryInitializedA = false;
        taaHistoryInitializedB = false;
        iblInitialized = false;
        iblImageInitialized = false;
        virtualHiZInitialized = false;
        virtualHiZImageInitialized = false;
        virtualVisibilityValid = false;
        virtualGeometryActive = false;
        meshShaderFallbackReason.clear();
        activeRenderPath = Halcyon::Renderer::Scene::RenderPathMode::DeferredIndexed;
        hasRenderedFrame = false;
        lastFrameIndex = 0;
        renderSerial = 0;
        previousInstances.clear();
        previousViewProjection = glm::mat4{1.0f};
        previousPacketValid = false;
        pendingScreenshotPath.clear();
        gpuSceneContentHash = 0;
        gpuSceneInstanceCount = 0;
        gpuMaterialCount = 0;
        materialDescriptorBindCount = 0;
        bindlessTextureHandles.clear();
        bindlessSamplerHandle = {};
        bindlessMaterialRows.clear();
        gpuSceneState.reset(131072);
    }

    [[nodiscard]] VoidResult createTimelineSemaphore()
    {
        return frameContext.createTimeline(device);
    }

    [[nodiscard]] VoidResult createFrameResources()
    {
        const VoidResult result = frameContext.createResources(
            device, physicalDevice, physicalProperties, graphicsQueueFamily, config.framesInFlight);
        timestampsEnabled = result && physicalProperties.limits.timestampPeriod > 0.0f;
        if (!result) return result;
        for (auto& readback : clusterOverflowReadbacks)
            gpuAllocator.destroy(readback);
        for (auto& readback : gpuVisibilityReadbacks)
            gpuAllocator.destroy(readback);
        for (auto& readback : virtualGeometryReadbacks)
            gpuAllocator.destroy(readback);
        for (auto& readback : virtualPageRequestReadbacks)
            gpuAllocator.destroy(readback);
        clusterOverflowReadbacks.clear();
        clusterOverflowReadbacks.reserve(frames.size());
        gpuVisibilityReadbacks.clear();
        gpuVisibilityReadbacks.reserve(frames.size());
        gpuVisibilityValid.assign(frames.size(), false);
        virtualGeometryReadbacks.clear();
        virtualGeometryReadbacks.reserve(frames.size());
        virtualGeometryValid.assign(frames.size(), false);
        virtualPageRequestReadbacks.clear();
        virtualPageRequestReadbacks.reserve(frames.size());
        virtualPageRequestValid.assign(frames.size(), false);
        virtualPageRequestFrameIndices.assign(frames.size(), 0u);
        virtualPageRequestPageCounts.assign(frames.size(), 0u);
        virtualPageRequestMeshIds.assign(frames.size(),
            std::numeric_limits<std::uint32_t>::max());
        gpuReferenceVisible.assign(frames.size(), {});
        instanceIdReadbacks.assign(frames.size(), {});
        instanceIdReadbackValid.assign(frames.size(), false);
        instanceIdReadbackFrameIndices.assign(frames.size(), 0);
        frameUploadBuffers.clear();
        frameUploadBuffers.resize(frames.size());
        for (std::size_t i = 0; i < frames.size(); ++i)
        {
            VkBufferCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            info.size = sizeof(std::uint32_t);
            info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            const auto allocation = gpuAllocator.createBuffer(info, MemoryUsage::GpuToCpu);
            if (!allocation)
            {
                for (auto& readback : clusterOverflowReadbacks)
                    gpuAllocator.destroy(readback);
                clusterOverflowReadbacks.clear();
                for (auto& readback : gpuVisibilityReadbacks)
                    gpuAllocator.destroy(readback);
                gpuVisibilityReadbacks.clear();
                for (auto& readback : virtualGeometryReadbacks)
                    gpuAllocator.destroy(readback);
                virtualGeometryReadbacks.clear();
                for (auto& readback : virtualPageRequestReadbacks)
                    gpuAllocator.destroy(readback);
                virtualPageRequestReadbacks.clear();
                return allocation.error();
            }
            clusterOverflowReadbacks.push_back(allocation.value());
            // Header: frustum candidate count, phase-1 visible count, and
            // phase-2 visible count. The two following fixed-capacity regions
            // contain the slot indices, allowing a completed frame to be
            // compared against the CPU reference without stalling the
            // rendering frame.
            info.size = sizeof(std::uint32_t) *
                (VisibilityReadbackHeaderCount + 2u * VisibilityReadbackCapacity);
            const auto visibility = gpuAllocator.createBuffer(info, MemoryUsage::GpuToCpu);
            if (!visibility)
            {
                for (auto& readback : clusterOverflowReadbacks)
                    gpuAllocator.destroy(readback);
                clusterOverflowReadbacks.clear();
                for (auto& readback : gpuVisibilityReadbacks)
                    gpuAllocator.destroy(readback);
                gpuVisibilityReadbacks.clear();
                for (auto& readback : virtualGeometryReadbacks)
                    gpuAllocator.destroy(readback);
                virtualGeometryReadbacks.clear();
                for (auto& readback : virtualPageRequestReadbacks)
                    gpuAllocator.destroy(readback);
                virtualPageRequestReadbacks.clear();
                return visibility.error();
            }
            gpuVisibilityReadbacks.push_back(visibility.value());
            // Visible meshlets, actual indirect commands, invalid visibility
            // records, selected DAG nodes, and completed LOD transitions.
            info.size = sizeof(std::uint32_t) * 5u;
            const auto virtualCounters = gpuAllocator.createBuffer(info, MemoryUsage::GpuToCpu);
            if (!virtualCounters)
            {
                for (auto& readback : clusterOverflowReadbacks)
                    gpuAllocator.destroy(readback);
                clusterOverflowReadbacks.clear();
                for (auto& readback : gpuVisibilityReadbacks)
                    gpuAllocator.destroy(readback);
                gpuVisibilityReadbacks.clear();
                for (auto& readback : virtualGeometryReadbacks)
                    gpuAllocator.destroy(readback);
                virtualGeometryReadbacks.clear();
                for (auto& readback : virtualPageRequestReadbacks)
                    gpuAllocator.destroy(readback);
                virtualPageRequestReadbacks.clear();
                return virtualCounters.error();
            }
            virtualGeometryReadbacks.push_back(virtualCounters.value());
            info.size = sizeof(std::uint32_t) *
                (1ull + DebugReadbackManager::VirtualPageRequestCapacity);
            const auto pageRequests = gpuAllocator.createBuffer(info, MemoryUsage::GpuToCpu);
            if (!pageRequests)
            {
                for (auto& readback : clusterOverflowReadbacks)
                    gpuAllocator.destroy(readback);
                clusterOverflowReadbacks.clear();
                for (auto& readback : gpuVisibilityReadbacks)
                    gpuAllocator.destroy(readback);
                gpuVisibilityReadbacks.clear();
                for (auto& readback : virtualGeometryReadbacks)
                    gpuAllocator.destroy(readback);
                virtualGeometryReadbacks.clear();
                for (auto& readback : virtualPageRequestReadbacks)
                    gpuAllocator.destroy(readback);
                virtualPageRequestReadbacks.clear();
                return pageRequests.error();
            }
            virtualPageRequestReadbacks.push_back(pageRequests.value());
        }
        return ok();
    }

    [[nodiscard]] VoidResult createRenderPipelines()
    {
        const auto graphicsResult = createGraphicsPipeline();
        if (!graphicsResult)
            return graphicsResult;
        const auto gpuResult = createGpuDrivenPipelines();
        if (gpuResult)
            return gpuResult;

        const bool meshPath = config.renderPath ==
            Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryMeshShader;
        const bool indexedPath = config.renderPath ==
            Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryIndexed;
        if (!meshPath && !indexedPath)
            return gpuResult;
        if (meshPath && config.meshShader == FeatureMode::Required)
            return gpuResult;

        // Virtual Geometry backends are optional in Auto mode. Tear down a
        // partially-created registry before retrying the next supported path.
        pipelines.destroySwapchainResources(device);
        gpuDrivenBindless = false;
        if (meshPath)
        {
            const std::string meshFailure = gpuResult.error().describe();
            config.renderPath = Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryIndexed;
            meshShaderFallbackReason =
                "Mesh Shader pipeline creation failed (" + meshFailure +
                "); using VirtualGeometryIndexed";
            setError(meshShaderFallbackReason);
            const auto fallbackGraphics = createGraphicsPipeline();
            if (!fallbackGraphics)
                return fallbackGraphics;
            const auto indexedResult = createGpuDrivenPipelines();
            if (indexedResult)
                return indexedResult;
            pipelines.destroySwapchainResources(device);
            gpuDrivenBindless = false;
        }
        const bool gpuFallback = config.enableGpuDrivenScene;
        config.renderPath = gpuFallback
            ? Halcyon::Renderer::Scene::RenderPathMode::GpuDrivenIndexed
            : Halcyon::Renderer::Scene::RenderPathMode::DeferredIndexed;
        setError(gpuFallback
            ? "VirtualGeometryIndexed pipeline creation failed; fell back to GpuDrivenIndexed"
            : "VirtualGeometryIndexed pipeline creation failed; fell back to DeferredIndexed");
        const auto fallbackGraphics = createGraphicsPipeline();
        if (!fallbackGraphics)
            return fallbackGraphics;
        return createGpuDrivenPipelines();
    }

    [[nodiscard]] VoidResult createSwapchain()
    {
        const VoidResult result = swapchainState.create();
        if (!result)
        {
            return result;
        }
        const VoidResult resourceResult = frameResources.recreate(swapchainExtent);
        if (!resourceResult)
        {
            return resourceResult;
        }
        const VoidResult pipelineResult = createRenderPipelines();
        deviceMemoryBytes = gpuAllocator.allocatedBytes();
        return pipelineResult;
    }

    [[nodiscard]] VoidResult recreateSwapchain()
    {
        const VoidResult result = swapchainState.recreate();
        deviceLost = deviceLost || swapchainState.deviceLost;
        if (!result)
        {
            return result;
        }
        // FrameGraph persistent images (TAA history and procedural IBL) are
        // keyed by their descriptor, which includes the swapchain extent for
        // history targets.  The swapchain recreation waits for the device,
        // so it is now safe to retire and rebuild those native allocations
        // before the next graph materialization.  Keeping the old cache here
        // would alias a resized history image with the previous extent.
        frameGraphProvider.recreatePersistent();
        const VoidResult resourceResult = frameResources.recreate(swapchainExtent);
        if (!resourceResult)
        {
            return resourceResult;
        }
        const VoidResult pipelineResult = createRenderPipelines();
        deviceMemoryBytes = gpuAllocator.allocatedBytes();
        // A swapchain resize changes the sampling footprint, so any temporal
        // history must be discarded before the next rendered frame.
        taaHistoryValid = false;
        taaHistoryInitializedA = false;
        taaHistoryInitializedB = false;
        iblInitialized = false;
        iblImageInitialized = false;
        virtualHiZInitialized = false;
        virtualHiZImageInitialized = false;
        virtualVisibilityValid = false;
        hasRenderedFrame = false;
        previousPacketValid = false;
        previousInstances.clear();
        return pipelineResult;
    }

    [[nodiscard]] VoidResult createGraphicsPipeline()
    {
        const std::array<DescriptorBindingDesc, 7> materialAbi = {
            DescriptorBindingDesc{0, {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {10, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {30, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}}};
        const std::array<DescriptorBindingDesc, 13> lightingAbi = {
            DescriptorBindingDesc{0, {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {6, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {7, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {10, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {20, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {21, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {22, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {23, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}}};
        const std::array<DescriptorBindingDesc, 2> tonemapAbi = {
            DescriptorBindingDesc{0, {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {10, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}}};
        const std::array<DescriptorBindingDesc, 5> taaAbi = {
            DescriptorBindingDesc{0, {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {10, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {20, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};
        const std::array<DescriptorBindingDesc, 5> clusterAbi = {
            DescriptorBindingDesc{0, {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};
        const std::array<VkFormat, 5> gbufferFormats = {
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_FORMAT_R16G16_SFLOAT,
            VK_FORMAT_R32_UINT};
        GraphicsPipelineDesc gbufferDesc{};
        gbufferDesc.colorFormats = gbufferFormats;
        gbufferDesc.depthFormat = depthFormat;
        gbufferDesc.cullMode = VK_CULL_MODE_BACK_BIT;
        gbufferDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&materialLayout, 1};
        gbufferDesc.descriptorBindings = materialAbi;
        const std::array<VkPushConstantRange, 1> gbufferPushRanges = {
            VkPushConstantRange{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(OpaquePushConstants)}};
        gbufferDesc.pushConstants = gbufferPushRanges;
        gbufferDesc.vertexShader = "pbr.vert.spv";
        gbufferDesc.fragmentShader = "gbuffer.frag.spv";
        const auto gbufferResult = gbufferPipeline.createGraphics(device, gbufferDesc);
        if (!gbufferResult) return gbufferResult;
        GraphicsPipelineDesc gbufferDoubleDesc = gbufferDesc;
        gbufferDoubleDesc.cullMode = VK_CULL_MODE_NONE;
        const auto gbufferDoubleResult = gbufferDoubleSidedPipeline.createGraphics(device, gbufferDoubleDesc);
        if (!gbufferDoubleResult) return gbufferDoubleResult;

        const std::array<VkFormat, 1> hdrFormat = {VK_FORMAT_R32G32B32A32_SFLOAT};
        GraphicsPipelineDesc deferredDesc{};
        deferredDesc.colorFormats = hdrFormat;
        deferredDesc.depthFormat = VK_FORMAT_UNDEFINED;
        deferredDesc.depthTest = false;
        deferredDesc.depthWrite = false;
        deferredDesc.cullMode = VK_CULL_MODE_NONE;
        deferredDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&lightingLayout, 1};
        deferredDesc.descriptorBindings = lightingAbi;
        const std::array<VkPushConstantRange, 1> deferredPushRange = {
            VkPushConstantRange{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128}};
        deferredDesc.pushConstants = deferredPushRange;
        deferredDesc.vertexShader = "fullscreen.vert.spv";
        deferredDesc.fragmentShader = "pbr.frag.spv";
        const auto deferredResult = deferredLightingPipeline.createGraphics(device, deferredDesc);
        if (!deferredResult) return deferredResult;

        GraphicsPipelineDesc transparentDesc = deferredDesc;
        transparentDesc.depthFormat = depthFormat;
        transparentDesc.depthTest = true;
        transparentDesc.depthWrite = false;
        transparentDesc.blendEnable = true;
        // Transparency has a distinct push-constant ABI (camera/light data
        // precedes the model matrix), so it must use its matching vertex
        // stage instead of reinterpreting the opaque pbr.vert constants.
        transparentDesc.vertexShader = "forward_transparent.vert.spv";
        transparentDesc.fragmentShader = "forward_transparent.frag.spv";
        transparentDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&materialLayout, 1};
        transparentDesc.descriptorBindings = materialAbi;
        const std::array<VkPushConstantRange, 1> transparentPushRange = {
            VkPushConstantRange{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(TransparentPushConstants)}};
        transparentDesc.pushConstants = transparentPushRange;
        const auto transparentResult = transparentPipeline.createGraphics(device, transparentDesc);
        if (!transparentResult) return transparentResult;
        GraphicsPipelineDesc transparentDoubleDesc = transparentDesc;
        transparentDoubleDesc.cullMode = VK_CULL_MODE_NONE;
        const auto transparentDoubleResult = transparentDoubleSidedPipeline.createGraphics(device, transparentDoubleDesc);
        if (!transparentDoubleResult) return transparentDoubleResult;

        GraphicsPipelineDesc tonemapDesc{};
        tonemapDesc.colorFormats = std::span<const VkFormat>{&swapchainFormat, 1};
        tonemapDesc.depthFormat = VK_FORMAT_UNDEFINED;
        tonemapDesc.depthTest = false;
        tonemapDesc.depthWrite = false;
        tonemapDesc.cullMode = VK_CULL_MODE_NONE;
        tonemapDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&tonemapLayout, 1};
        tonemapDesc.descriptorBindings = tonemapAbi;
        const std::array<VkPushConstantRange, 1> tonemapPushRange = {
            VkPushConstantRange{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16}};
        tonemapDesc.pushConstants = tonemapPushRange;
        tonemapDesc.vertexShader = "fullscreen.vert.spv";
        tonemapDesc.fragmentShader = "tonemap.frag.spv";
        const auto tonemapResult = tonemapPipeline.createGraphics(device, tonemapDesc);
        if (!tonemapResult) return tonemapResult;

        GraphicsPipelineDesc csmDesc{};
        csmDesc.depthOnly = true;
        csmDesc.depthFormat = VK_FORMAT_D32_SFLOAT;
        csmDesc.depthTest = true;
        csmDesc.depthWrite = true;
        csmDesc.depthCompare = VK_COMPARE_OP_GREATER_OR_EQUAL;
        csmDesc.cullMode = VK_CULL_MODE_BACK_BIT;
        csmDesc.depthBiasEnable = true;
        csmDesc.depthBiasConstant = -1.25f;
        csmDesc.depthBiasSlope = -1.75f;
        const std::array<VkPushConstantRange, 1> csmPushRange = {
            VkPushConstantRange{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4) * 2}};
        csmDesc.pushConstants = csmPushRange;
        csmDesc.vertexShader = "csm.vert.spv";
        const auto csmResult = csmDepthPipeline.createGraphics(device, csmDesc);
        if (!csmResult) return csmResult;

        ComputePipelineDesc taaDesc{};
        taaDesc.shader = "taa.comp.spv";
        taaDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&taaLayout, 1};
        taaDesc.descriptorBindings = taaAbi;
        const std::array<VkPushConstantRange, 1> taaPushRange = {
            VkPushConstantRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16}};
        taaDesc.pushConstants = taaPushRange;
        const auto taaResult = taaPipeline.createCompute(device, taaDesc);
        if (!taaResult) return taaResult;
        ComputePipelineDesc clusterDesc{};
        clusterDesc.shader = "cluster_build.comp.spv";
        clusterDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&clusterLayout, 1};
        clusterDesc.descriptorBindings = clusterAbi;
        const std::array<VkPushConstantRange, 1> clusterPushRange = {
            VkPushConstantRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, 48}};
        clusterDesc.pushConstants = clusterPushRange;
        const auto clusterResult = clusterBuildPipeline.createCompute(device, clusterDesc);
        if (!clusterResult) return clusterResult;
        return ok();
    }

    [[nodiscard]] VoidResult createGpuDrivenPipelines()
    {
        // Virtual Geometry shares the GPU scene descriptor/pipeline lifetime,
        // but it is a distinct render path.  Keep the renderer usable when a
        // caller selects it directly without also setting the legacy GPU
        // driven toggle.
        const bool virtualGeometryPath =
            config.renderPath == Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryIndexed ||
            config.renderPath == Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryMeshShader;
        if (!config.enableGpuDrivenScene && !virtualGeometryPath)
            return ok();
        const auto makeLayout = [&](std::span<const VkDescriptorSetLayoutBinding> bindings,
                                    VkDescriptorSetLayout& output) -> VoidResult
        {
            VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            info.bindingCount = static_cast<std::uint32_t>(bindings.size());
            info.pBindings = bindings.data();
            return vkCreateDescriptorSetLayout(device, &info, nullptr, &output) == VK_SUCCESS
                       ? ok()
                       : fail("failed to create GPU scene descriptor layout");
        };
        const std::array<VkDescriptorSetLayoutBinding, 5> cullBindings = {
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        const auto indirectBindings = kIndirectBuildLayoutBindings;
        auto result = makeLayout(cullBindings, gpuSceneCullLayout);
        if (!result) return result;
        result = makeLayout(indirectBindings, gpuSceneIndirectLayout);
        if (!result) return result;
        const std::array<VkDescriptorSetLayoutBinding, 4> graphicsBindings = {
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                VK_SHADER_STAGE_VERTEX_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                VK_SHADER_STAGE_VERTEX_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                VK_SHADER_STAGE_VERTEX_BIT, nullptr},
            VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
        result = makeLayout(graphicsBindings, gpuSceneGraphicsLayout);
        if (!result) return result;
        const std::array<VkDescriptorSetLayoutBinding, 2> csmGraphicsBindings = {
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                VK_SHADER_STAGE_VERTEX_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
        result = makeLayout(csmGraphicsBindings, gpuCsmGraphicsLayout);
        if (!result) return result;
        const std::array<VkDescriptorPoolSize, 1> poolSizes = {
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16}};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 8;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = poolSizes.data();
        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &gpuSceneDescriptorPool) != VK_SUCCESS)
            return fail("failed to create GPU scene descriptor pool");
        const std::array<DescriptorBindingDesc, 5> cullAbi = {
            DescriptorBindingDesc{0, {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};
        ComputePipelineDesc cullDesc{};
        cullDesc.shader = "frustum_cull.comp.spv";
        cullDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&gpuSceneCullLayout, 1};
        cullDesc.descriptorBindings = cullAbi;
        const std::array<VkPushConstantRange, 1> cullPush = {{
            {VK_SHADER_STAGE_COMPUTE_BIT, 0, 112}}};
        cullDesc.pushConstants = cullPush;
        result = frustumCullPipeline.createCompute(device, cullDesc);
        if (!result) return result;
        const bool useBindless = bindlessTable.initialized() &&
            bindlessTable.table().capacity(Resources::DescriptorType::SampledImage) >=
                kGpuDrivenSampledImageCapacity &&
            bindlessTable.table().capacity(Resources::DescriptorType::Sampler) >=
                kGpuDrivenSamplerCapacity;
        std::array<VkDescriptorSetLayout, 2> gpuGraphicsLayouts =
            {useBindless ? bindlessTable.layout() : materialLayout, gpuSceneGraphicsLayout};
        std::array<DescriptorBindingDesc, 10> gpuGraphicsAbi{};
        if (useBindless)
        {
            gpuGraphicsAbi = {
                DescriptorBindingDesc{0, {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                    kGpuDrivenSampledImageCapacity,
                    VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
                DescriptorBindingDesc{0, {4, VK_DESCRIPTOR_TYPE_SAMPLER,
                    kGpuDrivenSamplerCapacity,
                    VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
                DescriptorBindingDesc{1, {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                    VK_SHADER_STAGE_VERTEX_BIT, nullptr}},
                DescriptorBindingDesc{1, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                    VK_SHADER_STAGE_VERTEX_BIT, nullptr}},
                DescriptorBindingDesc{1, {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                    VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
                DescriptorBindingDesc{1, {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                    VK_SHADER_STAGE_VERTEX_BIT, nullptr}},
                {}, {}, {}, {}};
        }
        else
        {
            for (std::uint32_t i = 0; i < 7; ++i)
                gpuGraphicsAbi[i] = DescriptorBindingDesc{0,
                    {i < 5 ? i : (i == 5 ? 10u : 30u),
                        i == 5 ? VK_DESCRIPTOR_TYPE_SAMPLER
                               : (i == 6 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                         : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
                        1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
            gpuGraphicsAbi[7] = DescriptorBindingDesc{1,
                {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
            gpuGraphicsAbi[8] = DescriptorBindingDesc{1,
                {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
            gpuGraphicsAbi[9] = DescriptorBindingDesc{1,
                {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
        }
        GraphicsPipelineDesc gpuGraphics{};
        const std::array<VkFormat, 5> gpuFormats = {VK_FORMAT_R8G8B8A8_UNORM,
            VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM,
            VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R32_UINT};
        gpuGraphics.colorFormats = gpuFormats;
        gpuGraphics.depthFormat = depthFormat;
        gpuGraphics.descriptorLayouts = gpuGraphicsLayouts;
        gpuGraphics.descriptorBindings = gpuGraphicsAbi;
        const std::array<VkPushConstantRange, 1> gpuPush = {{
            {VK_SHADER_STAGE_VERTEX_BIT, 0, 128}}};
        gpuGraphics.pushConstants = gpuPush;
        gpuGraphics.vertexShader = "gpu_driven.vert.spv";
        gpuGraphics.fragmentShader = useBindless ? "gpu_driven.frag.spv" : "gbuffer.frag.spv";
        gpuGraphics.cullMode = VK_CULL_MODE_BACK_BIT;
        result = gpuDrivenGbufferPipeline.createGraphics(device, gpuGraphics);
        if (!result) return result;
        GraphicsPipelineDesc gpuGraphicsDouble = gpuGraphics;
        gpuGraphicsDouble.cullMode = VK_CULL_MODE_NONE;
        result = gpuDrivenGbufferDoubleSidedPipeline.createGraphics(device, gpuGraphicsDouble);
        if (!result) return result;
        gpuDrivenBindless = useBindless;
        GraphicsPipelineDesc gpuCsmDesc{};
        gpuCsmDesc.depthOnly = true;
        gpuCsmDesc.depthFormat = VK_FORMAT_D32_SFLOAT;
        gpuCsmDesc.depthTest = true;
        gpuCsmDesc.depthWrite = true;
        gpuCsmDesc.depthCompare = VK_COMPARE_OP_GREATER_OR_EQUAL;
        gpuCsmDesc.cullMode = VK_CULL_MODE_BACK_BIT;
        gpuCsmDesc.depthBiasEnable = true;
        gpuCsmDesc.depthBiasConstant = -1.25f;
        gpuCsmDesc.depthBiasSlope = -1.75f;
        gpuCsmDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&gpuCsmGraphicsLayout, 1};
        const std::array<DescriptorBindingDesc, 2> gpuCsmAbi = {
            DescriptorBindingDesc{0, {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                VK_SHADER_STAGE_VERTEX_BIT, nullptr}},
            DescriptorBindingDesc{0, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                VK_SHADER_STAGE_VERTEX_BIT, nullptr}}};
        gpuCsmDesc.descriptorBindings = gpuCsmAbi;
        const std::array<VkPushConstantRange, 1> gpuCsmPush = {{
            {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4)}}};
        gpuCsmDesc.pushConstants = gpuCsmPush;
        gpuCsmDesc.vertexShader = "gpu_driven_csm.vert.spv";
        result = gpuDrivenCsmPipeline.createGraphics(device, gpuCsmDesc);
        if (!result) return result;
        const auto indirectAbi = indirectBuildAbi();
        ComputePipelineDesc indirectDesc{};
        indirectDesc.shader = "build_indirect_commands.comp.spv";
        indirectDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&gpuSceneIndirectLayout, 1};
        indirectDesc.descriptorBindings = indirectAbi;
        const std::array<VkPushConstantRange, 1> indirectPush = {{{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16}}};
        indirectDesc.pushConstants = indirectPush;
        result = indirectBuildPipeline.createCompute(device, indirectDesc);
        if (!result) return result;
        const std::array<VkDescriptorSetLayoutBinding, 2> hizBindings = {
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        result = makeLayout(hizBindings, hizLayout);
        if (!result) return result;
        const std::array<DescriptorBindingDesc, 2> hizAbi = {
            DescriptorBindingDesc{0, {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};
        ComputePipelineDesc hizDesc{};
        hizDesc.shader = "hiz_build.comp.spv";
        hizDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&hizLayout, 1};
        hizDesc.descriptorBindings = hizAbi;
        const std::array<VkPushConstantRange, 1> hizPush = {{{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16}}};
        hizDesc.pushConstants = hizPush;
        result = hizBuildPipeline.createCompute(device, hizDesc);
        if (!result) return result;

        // Two-phase occlusion uses the frustum-visible list as candidates,
        // then classifies those candidates against the previous/current Hi-Z
        // pyramid.  Keep the layouts explicit so descriptor ABI validation
        // catches shader changes at pipeline creation time.
        const std::array<VkDescriptorSetLayoutBinding, 8> phase1Bindings = {
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        result = makeLayout(phase1Bindings, occlusionPhase1Layout);
        if (!result) return result;
        const std::array<DescriptorBindingDesc, 8> phase1Abi = {
            DescriptorBindingDesc{0, {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};
        ComputePipelineDesc phase1Desc{};
        phase1Desc.shader = "occlusion_phase1.comp.spv";
        phase1Desc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&occlusionPhase1Layout, 1};
        phase1Desc.descriptorBindings = phase1Abi;
        const std::array<VkPushConstantRange, 1> occlusionPush = {{{VK_SHADER_STAGE_COMPUTE_BIT, 0, 96}}};
        phase1Desc.pushConstants = occlusionPush;
        result = occlusionPhase1Pipeline.createCompute(device, phase1Desc);
        if (!result) return result;

        const std::array<VkDescriptorSetLayoutBinding, 6> phase2Bindings = {
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        result = makeLayout(phase2Bindings, occlusionPhase2Layout);
        if (!result) return result;
        const std::array<DescriptorBindingDesc, 6> phase2Abi = {
            DescriptorBindingDesc{0, {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};
        ComputePipelineDesc phase2Desc{};
        phase2Desc.shader = "occlusion_phase2.comp.spv";
        phase2Desc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&occlusionPhase2Layout, 1};
        phase2Desc.descriptorBindings = phase2Abi;
        phase2Desc.pushConstants = occlusionPush;
        result = occlusionPhase2Pipeline.createCompute(device, phase2Desc);
        if (!result) return result;

        // The visibility/material/shading ABI is only needed by the M5 path.
        // Keep it out of legacy GPU-driven initialization so an unavailable
        // M5 shader or attachment capability cannot break GpuDrivenIndexed.
        if (!virtualGeometryPath)
            return ok();

        const std::array<VkDescriptorSetLayoutBinding, 13> meshletCullBindings = {
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{12, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        result = makeLayout(meshletCullBindings, meshletCullLayout);
        if (!result) return result;
        const std::array<DescriptorBindingDesc, 13> meshletCullAbi = {
            DescriptorBindingDesc{0, meshletCullBindings[0], sizeof(VulkanSceneResources::VirtualGeometryGpuMeshlet)},
            DescriptorBindingDesc{0, meshletCullBindings[1], sizeof(std::uint32_t)},
            DescriptorBindingDesc{0, meshletCullBindings[2], sizeof(std::uint32_t)},
            DescriptorBindingDesc{0, meshletCullBindings[3]},
            DescriptorBindingDesc{0, meshletCullBindings[4], sizeof(Halcyon::Renderer::Scene::TransformRow)},
            DescriptorBindingDesc{0, meshletCullBindings[5]},
            DescriptorBindingDesc{0, meshletCullBindings[6], sizeof(VulkanSceneResources::VirtualGeometryGpuDagNode)},
            DescriptorBindingDesc{0, meshletCullBindings[7], sizeof(VulkanSceneResources::VirtualGeometryGpuCluster)},
            DescriptorBindingDesc{0, meshletCullBindings[8], sizeof(std::uint32_t)},
            DescriptorBindingDesc{0, meshletCullBindings[9], sizeof(std::uint32_t)},
            DescriptorBindingDesc{0, meshletCullBindings[10], sizeof(std::uint32_t)},
            DescriptorBindingDesc{0, meshletCullBindings[11], sizeof(std::uint32_t)},
            DescriptorBindingDesc{0, meshletCullBindings[12], 16u}};
        ComputePipelineDesc meshletCullDesc{};
        meshletCullDesc.shader = "meshlet_cull.comp.spv";
        meshletCullDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&meshletCullLayout, 1};
        meshletCullDesc.descriptorBindings = meshletCullAbi;
        const std::array<VkPushConstantRange, 1> meshletCullPush = {{{VK_SHADER_STAGE_COMPUTE_BIT, 0, 128}}};
        meshletCullDesc.pushConstants = meshletCullPush;
        result = meshletCullPipeline.createCompute(device, meshletCullDesc);
        if (!result) return result;

        const std::array<VkDescriptorSetLayoutBinding, 13> lodSelectBindings = {
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{12, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        result = makeLayout(lodSelectBindings, lodSelectLayout);
        if (!result) return result;
        const std::array<DescriptorBindingDesc, 13> lodSelectAbi = {
            DescriptorBindingDesc{0, lodSelectBindings[0], sizeof(VulkanSceneResources::VirtualGeometryGpuDagNode)},
            DescriptorBindingDesc{0, lodSelectBindings[1], sizeof(Halcyon::Renderer::Scene::VirtualGeometryDagEdge)},
            DescriptorBindingDesc{0, lodSelectBindings[2], 16},
            DescriptorBindingDesc{0, lodSelectBindings[3], sizeof(std::uint32_t)},
            DescriptorBindingDesc{0, lodSelectBindings[4], sizeof(std::uint32_t)},
            DescriptorBindingDesc{0, lodSelectBindings[5], sizeof(std::uint32_t)},
            DescriptorBindingDesc{0, lodSelectBindings[6], sizeof(std::uint32_t)},
            DescriptorBindingDesc{0, lodSelectBindings[7], sizeof(std::uint32_t)},
            DescriptorBindingDesc{0, lodSelectBindings[8], sizeof(VulkanSceneResources::VirtualGeometryGpuPageTableEntry)},
            DescriptorBindingDesc{0, lodSelectBindings[9], sizeof(Halcyon::Renderer::Scene::VirtualGeometryPageDependencyRange)},
            DescriptorBindingDesc{0, lodSelectBindings[10], sizeof(std::uint32_t)},
            DescriptorBindingDesc{0, lodSelectBindings[11], sizeof(std::uint32_t)},
            DescriptorBindingDesc{0, lodSelectBindings[12], sizeof(std::uint32_t)}};
        ComputePipelineDesc lodSelectDesc{};
        lodSelectDesc.shader = "lod_select.comp.spv";
        lodSelectDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&lodSelectLayout, 1};
        lodSelectDesc.descriptorBindings = lodSelectAbi;
        const std::array<VkPushConstantRange, 1> lodSelectPush = {{{VK_SHADER_STAGE_COMPUTE_BIT, 0, 128}}};
        lodSelectDesc.pushConstants = lodSelectPush;
        result = lodSelectPipeline.createCompute(device, lodSelectDesc);
        if (!result) return result;

        const std::array<VkDescriptorSetLayoutBinding, 5> meshletIndirectBindings = {
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        result = makeLayout(meshletIndirectBindings, meshletIndirectLayout);
        if (!result) return result;
        const std::array<DescriptorBindingDesc, 5> meshletIndirectAbi = {
            DescriptorBindingDesc{0, meshletIndirectBindings[0], sizeof(std::uint32_t)},
            DescriptorBindingDesc{0, meshletIndirectBindings[1], sizeof(VulkanSceneResources::VirtualGeometryGpuMeshlet)},
            DescriptorBindingDesc{0, meshletIndirectBindings[2], sizeof(std::uint32_t)},
            DescriptorBindingDesc{0, meshletIndirectBindings[3], sizeof(VkDrawIndexedIndirectCommand)},
            DescriptorBindingDesc{0, meshletIndirectBindings[4], sizeof(std::uint32_t)}};
        ComputePipelineDesc meshletIndirectDesc{};
        meshletIndirectDesc.shader = "meshlet_build_indirect.comp.spv";
        meshletIndirectDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&meshletIndirectLayout, 1};
        meshletIndirectDesc.descriptorBindings = meshletIndirectAbi;
        const std::array<VkPushConstantRange, 1> meshletIndirectPush = {{{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16}}};
        meshletIndirectDesc.pushConstants = meshletIndirectPush;
        result = meshletIndirectPipeline.createCompute(device, meshletIndirectDesc);
        if (!result) return result;

        const std::array<VkDescriptorSetLayoutBinding, 4> meshMeshIndirectBindings = {
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        result = makeLayout(meshMeshIndirectBindings, meshletMeshIndirectLayout);
        if (!result) return result;
        const std::array<DescriptorBindingDesc, 4> meshMeshIndirectAbi = {
            DescriptorBindingDesc{0, meshMeshIndirectBindings[0], sizeof(std::uint32_t)},
            DescriptorBindingDesc{0, meshMeshIndirectBindings[1], sizeof(std::uint32_t)},
            DescriptorBindingDesc{0, meshMeshIndirectBindings[2], 12u},
            DescriptorBindingDesc{0, meshMeshIndirectBindings[3], sizeof(std::uint32_t)}};
        ComputePipelineDesc meshMeshIndirectDesc{};
        meshMeshIndirectDesc.shader = "meshlet_build_mesh_indirect.comp.spv";
        meshMeshIndirectDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&meshletMeshIndirectLayout, 1};
        meshMeshIndirectDesc.descriptorBindings = meshMeshIndirectAbi;
        const std::array<VkPushConstantRange, 1> meshMeshIndirectPush = {{{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16}}};
        meshMeshIndirectDesc.pushConstants = meshMeshIndirectPush;
        result = meshletMeshIndirectPipeline.createCompute(device, meshMeshIndirectDesc);
        if (!result) return result;

        if (config.renderPath == Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryMeshShader &&
            caps.meshShader)
        {
            const std::array<VkDescriptorSetLayoutBinding, 8> meshBindings = {
                VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_MESH_BIT_EXT, nullptr},
                VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_MESH_BIT_EXT, nullptr},
                VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_MESH_BIT_EXT, nullptr},
                VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_MESH_BIT_EXT, nullptr},
                VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_MESH_BIT_EXT, nullptr},
                VkDescriptorSetLayoutBinding{5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_MESH_BIT_EXT, nullptr},
                VkDescriptorSetLayoutBinding{6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_MESH_BIT_EXT, nullptr},
                VkDescriptorSetLayoutBinding{7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_MESH_BIT_EXT, nullptr}};
            result = makeLayout(meshBindings, virtualGeometryMeshLayout);
            if (!result) return result;
            const std::array<DescriptorBindingDesc, 8> meshAbi = {
                DescriptorBindingDesc{0, meshBindings[0], sizeof(std::uint32_t)},
                DescriptorBindingDesc{0, meshBindings[1], sizeof(std::uint32_t)},
                DescriptorBindingDesc{0, meshBindings[2], sizeof(VulkanSceneResources::VirtualGeometryGpuMeshlet)},
                DescriptorBindingDesc{0, meshBindings[3], sizeof(std::uint32_t)},
                DescriptorBindingDesc{0, meshBindings[4]},
                DescriptorBindingDesc{0, meshBindings[5], sizeof(Halcyon::Renderer::Scene::StaticSceneVertex)},
                DescriptorBindingDesc{0, meshBindings[6], sizeof(Halcyon::Renderer::Scene::TransformRow)},
                DescriptorBindingDesc{0, meshBindings[7], sizeof(Halcyon::Renderer::Scene::MeshMaterialRow)}};
            GraphicsPipelineDesc meshDesc{};
            const std::array<VkFormat, 3> meshFormats = {
                VK_FORMAT_R32_UINT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_UINT};
            meshDesc.colorFormats = meshFormats;
            meshDesc.depthFormat = depthFormat;
            meshDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&virtualGeometryMeshLayout, 1};
            meshDesc.descriptorBindings = meshAbi;
            meshDesc.meshShader = "virtual_geometry_mesh.ms.spv";
            meshDesc.fragmentShader = "visibility_mesh.frag.spv";
            meshDesc.cullMode = VK_CULL_MODE_BACK_BIT;
            meshDesc.depthCompare = VK_COMPARE_OP_GREATER_OR_EQUAL;
            const std::array<VkPushConstantRange, 1> meshPush = {{{VK_SHADER_STAGE_MESH_BIT_EXT, 0, 80}}};
            meshDesc.pushConstants = meshPush;
            result = virtualGeometryMeshPipeline.createGraphics(device, meshDesc);
            if (!result) return result;
        }

        const std::array<VkDescriptorSetLayoutBinding, 6> visibilityBindings = {
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
            VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
            VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
            VkDescriptorSetLayoutBinding{5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
        result = makeLayout(visibilityBindings, visibilityLayout);
        if (!result) return result;
        const std::array<DescriptorBindingDesc, 6> visibilityAbi = {
            DescriptorBindingDesc{0, visibilityBindings[0], sizeof(VulkanSceneResources::VirtualGeometryGpuMeshlet)},
            DescriptorBindingDesc{0, visibilityBindings[1], sizeof(std::uint32_t)},
            DescriptorBindingDesc{0, visibilityBindings[2], sizeof(Halcyon::Renderer::Scene::StaticSceneVertex)},
            DescriptorBindingDesc{0, visibilityBindings[3], sizeof(std::uint32_t)},
            DescriptorBindingDesc{0, visibilityBindings[4], sizeof(Halcyon::Renderer::Scene::TransformRow)},
            DescriptorBindingDesc{0, visibilityBindings[5], sizeof(Halcyon::Renderer::Scene::MeshMaterialRow)}};
        GraphicsPipelineDesc visibilityDesc{};
        const std::array<VkFormat, 3> visibilityFormats = {
            VK_FORMAT_R32_UINT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_UINT};
        visibilityDesc.colorFormats = visibilityFormats;
        visibilityDesc.depthFormat = depthFormat;
        visibilityDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&visibilityLayout, 1};
        visibilityDesc.descriptorBindings = visibilityAbi;
        visibilityDesc.vertexShader = "visibility.vert.spv";
        visibilityDesc.fragmentShader = "visibility.frag.spv";
        // M5 accepts only static, single-sided materials. Keep the raster
        // state aligned with that classification so back-facing triangles do
        // not compete for visibility IDs or produce inverted normals.
        visibilityDesc.cullMode = VK_CULL_MODE_BACK_BIT;
        // Reversed-Z depth keeps the nearest meshlet/triangle and gives the
        // visibility buffer a deterministic front-most primitive.
        visibilityDesc.depthCompare = VK_COMPARE_OP_GREATER_OR_EQUAL;
        const std::array<VkPushConstantRange, 1> visibilityPush = {{
            {VK_SHADER_STAGE_VERTEX_BIT, 0, 80}}};
        visibilityDesc.pushConstants = visibilityPush;
        if (caps.fragmentBarycentric)
        {
            result = visibilityPipeline.createGraphics(device, visibilityDesc);
            if (!result) return result;
        }

        const std::array<VkDescriptorSetLayoutBinding, 7> classifyBindings = {
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        result = makeLayout(classifyBindings, materialClassifyLayout);
        if (!result) return result;
        const std::array<DescriptorBindingDesc, 7> classifyAbi = {
            DescriptorBindingDesc{0, classifyBindings[0]}, DescriptorBindingDesc{0, classifyBindings[1]},
            DescriptorBindingDesc{0, classifyBindings[2], sizeof(Halcyon::Renderer::Scene::MeshMaterialRow)},
            DescriptorBindingDesc{0, classifyBindings[3], sizeof(std::uint32_t)},
            DescriptorBindingDesc{0, classifyBindings[4], sizeof(std::uint32_t)},
            DescriptorBindingDesc{0, classifyBindings[5], sizeof(VulkanSceneResources::VirtualGeometryGpuMeshlet)},
            DescriptorBindingDesc{0, classifyBindings[6]}};
        ComputePipelineDesc classifyDesc{};
        classifyDesc.shader = "material_classify.comp.spv";
        classifyDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&materialClassifyLayout, 1};
        classifyDesc.descriptorBindings = classifyAbi;
        const std::array<VkPushConstantRange, 1> classifyPush = {{{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32}}};
        classifyDesc.pushConstants = classifyPush;
        result = materialClassifyPipeline.createCompute(device, classifyDesc);
        if (!result) return result;

        const std::array<VkDescriptorSetLayoutBinding, 16> shadingBindings = {
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{8, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{9, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{10, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{11, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{12, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{13, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{14, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{15, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        result = makeLayout(shadingBindings, computeShadingLayout);
        if (!result) return result;
        const std::array<DescriptorBindingDesc, 16> shadingAbi = {
            DescriptorBindingDesc{0, shadingBindings[0]}, DescriptorBindingDesc{0, shadingBindings[1]},
            DescriptorBindingDesc{0, shadingBindings[2]}, DescriptorBindingDesc{0, shadingBindings[3], sizeof(std::uint32_t)},
            DescriptorBindingDesc{0, shadingBindings[4]},
            DescriptorBindingDesc{0, shadingBindings[5], sizeof(VulkanSceneResources::VirtualGeometryGpuMeshlet)},
            DescriptorBindingDesc{0, shadingBindings[6], sizeof(std::uint32_t)},
            DescriptorBindingDesc{0, shadingBindings[7], sizeof(Halcyon::Renderer::Scene::StaticSceneVertex)},
            DescriptorBindingDesc{0, shadingBindings[8]}, DescriptorBindingDesc{0, shadingBindings[9]},
            DescriptorBindingDesc{0, shadingBindings[10]}, DescriptorBindingDesc{0, shadingBindings[11]},
            DescriptorBindingDesc{0, shadingBindings[12], sizeof(Halcyon::Renderer::Scene::MaterialGpuData)},
            DescriptorBindingDesc{0, shadingBindings[13], sizeof(Halcyon::Renderer::Scene::LightData)},
            DescriptorBindingDesc{0, shadingBindings[14], sizeof(Halcyon::Renderer::Scene::TransformRow)},
            DescriptorBindingDesc{0, shadingBindings[15]}};
        ComputePipelineDesc shadingDesc{};
        shadingDesc.shader = "compute_shading.comp.spv";
        shadingDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&computeShadingLayout, 1};
        shadingDesc.descriptorBindings = shadingAbi;
        const std::array<VkPushConstantRange, 1> shadingPush = {{
            {VK_SHADER_STAGE_COMPUTE_BIT, 0, 128}}};
        shadingDesc.pushConstants = shadingPush;
        return computeShadingPipeline.createCompute(device, shadingDesc);
    }

    [[nodiscard]] VoidResult createFrameDescriptors()
    {
        if (lightingLayout != VK_NULL_HANDLE)
            return ok();
        const auto createLayout = [&](std::span<const VkDescriptorSetLayoutBinding> bindings,
                                      VkDescriptorSetLayout& output) -> VoidResult
        {
            VkDescriptorSetLayoutCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            info.bindingCount = static_cast<std::uint32_t>(bindings.size());
            info.pBindings = bindings.data();
            const VkResult result = vkCreateDescriptorSetLayout(device, &info, nullptr, &output);
            return result == VK_SUCCESS ? ok() : fail(vkFailure("vkCreateDescriptorSetLayout", result));
        };
        std::array<VkDescriptorSetLayoutBinding, 7> materialBindings{};
        for (std::uint32_t i = 0; i < 5; ++i)
            materialBindings[i] = {i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        materialBindings[5] = {10, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        materialBindings[6] = {30, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        auto result = createLayout(materialBindings, materialLayout);
        if (!result) return result;
        std::array<VkDescriptorSetLayoutBinding, 13> lightingBindings{};
        for (std::uint32_t i = 0; i < 8; ++i)
            lightingBindings[i] = {i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        lightingBindings[8] = {10, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        lightingBindings[9] = {20, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        lightingBindings[10] = {21, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        lightingBindings[11] = {22, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        lightingBindings[12] = {23, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        result = createLayout(lightingBindings, lightingLayout);
        if (!result) return result;
        std::array<VkDescriptorSetLayoutBinding, 5> taaBindings = {
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{10, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{20, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        result = createLayout(taaBindings, taaLayout);
        if (!result) return result;
        std::array<VkDescriptorSetLayoutBinding, 5> clusterBindings = {
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        result = createLayout(clusterBindings, clusterLayout);
        if (!result) return result;
        const std::array<VkDescriptorSetLayoutBinding, 2> tonemapBindings = {
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{10, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
        result = createLayout(tonemapBindings, tonemapLayout);
        if (!result) return result;
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(device, &samplerInfo, nullptr, &linearSampler) != VK_SUCCESS)
            return fail("failed to create linear sampler");
        const std::array<VkDescriptorPoolSize, 5> poolSizes = {
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 128},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, 32},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 32},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 128},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64}};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 64;
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        const std::uint32_t poolCount = std::max(1u, config.framesInFlight);
        frameDescriptorPools.reserve(poolCount);
        for (std::uint32_t i = 0; i < poolCount; ++i)
        {
            VkDescriptorPool pool = VK_NULL_HANDLE;
            if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS)
            {
                cleanupFrameDescriptors();
                return fail("failed to create frame descriptor pool");
            }
            frameDescriptorPools.push_back(pool);
        }
        frameDescriptorPool = frameDescriptorPools.front();
        return ok();
    }

    [[nodiscard]] VoidResult recordFrame(
        FrameContext& frame, std::uint32_t imageIndex, const FramePacket& packet,
        VkBuffer screenshotReadback);

    [[nodiscard]] VoidResult recordImageUpload(VkCommandBuffer commandBuffer,
        VkImage image,
        std::span<const std::uint16_t> data,
        std::span<const VkBufferImageCopy> copies)
    {
        if (commandBuffer == VK_NULL_HANDLE || image == VK_NULL_HANDLE || data.empty() ||
            copies.empty() || currentFrame >= frameUploadBuffers.size())
        {
            return fail("invalid procedural IBL upload state",
                Halcyon::ErrorCode::InvalidState);
        }
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = data.size_bytes();
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        const auto allocation = gpuAllocator.createBuffer(info, MemoryUsage::CpuToGpu);
        if (!allocation)
        {
            return allocation.error();
        }
        BufferAllocation staging = allocation.value();
        const auto bytes = std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(data.data()), data.size_bytes()};
        const auto write = gpuAllocator.writeBuffer(staging, bytes);
        if (!write)
        {
            gpuAllocator.destroy(staging);
            return write.error();
        }
        try
        {
            frameUploadBuffers[currentFrame].push_back(staging);
        }
        catch (...)
        {
            gpuAllocator.destroy(staging);
            return fail("failed to retain procedural IBL staging allocation",
                Halcyon::ErrorCode::OutOfMemory);
        }
        vkCmdCopyBufferToImage(commandBuffer, staging.buffer, image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<std::uint32_t>(copies.size()),
            copies.data());
        return ok();
    }

    [[nodiscard]] VoidResult captureScreenshot(const std::filesystem::path& path)
    {
        if (!initialized || device == VK_NULL_HANDLE || path.empty())
        {
            return fail("A screenshot requires an initialized renderer and a non-empty path",
                Halcyon::ErrorCode::InvalidState);
        }
        if (!pendingScreenshotPath.empty())
        {
            return fail("A screenshot is already queued for the next rendered frame",
                Halcyon::ErrorCode::InvalidState);
        }
        pendingScreenshotPath = path;
        return ok();
    }

    [[nodiscard]] VoidResult writeScreenshot(
        const BufferAllocation& readback, const std::filesystem::path& path)
    {
        const VkDeviceSize size = static_cast<VkDeviceSize>(swapchainExtent.width) *
                                  static_cast<VkDeviceSize>(swapchainExtent.height) * 4u;
        auto bytes = gpuAllocator.readBuffer(readback, 0, size);
        if (!bytes)
        {
            return bytes.error();
        }
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(size));
        const auto* src = reinterpret_cast<const std::uint8_t*>(bytes.value().data());
        for (std::uint32_t y = 0; y < swapchainExtent.height; ++y)
        {
            for (std::uint32_t x = 0; x < swapchainExtent.width; ++x)
            {
                const std::size_t index =
                    (static_cast<std::size_t>(y) * swapchainExtent.width + x) * 4u;
                const bool bgra = swapchainFormat == VK_FORMAT_B8G8R8A8_SRGB ||
                                  swapchainFormat == VK_FORMAT_B8G8R8A8_UNORM;
                rgba[index + 0] = bgra ? src[index + 2] : src[index + 0];
                rgba[index + 1] = src[index + 1];
                rgba[index + 2] = bgra ? src[index + 0] : src[index + 2];
                rgba[index + 3] = src[index + 3];
            }
        }
        std::error_code error;
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path(), error);
        }
        if (stbi_write_png(path.string().c_str(),
                static_cast<int>(swapchainExtent.width),
                static_cast<int>(swapchainExtent.height),
                4,
                rgba.data(),
                static_cast<int>(swapchainExtent.width * 4u)) == 0)
        {
            return fail("Failed to write screenshot", Halcyon::ErrorCode::Io);
        }
        return ok();
    }

    [[nodiscard]] FrameStats render(const FramePacket& packet)
    {
        HALCYON_PROFILE_SCOPE("Renderer::render");
        FrameStats stats{};
        stats.quality.rayQueryEnabled = rayQueryEnabled;
        stats.quality.exposure = config.exposure;
        stats.quality.taaEnabled = config.enableTaa;
        stats.quality.clusteredLightingEnabled = config.enableClusteredLighting;
        stats.quality.transparencyEnabled = config.enableTransparency;
        switch (activeRenderPath)
        {
        case Halcyon::Renderer::Scene::RenderPathMode::DeferredIndexed:
            stats.renderPath = "DeferredIndexed";
            break;
        case Halcyon::Renderer::Scene::RenderPathMode::GpuDrivenIndexed:
            stats.renderPath = "GpuDrivenIndexed";
            break;
        case Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryIndexed:
            stats.renderPath = "VirtualGeometryIndexed";
            break;
        case Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryMeshShader:
            stats.renderPath = "VirtualGeometryMeshShader";
            break;
        }
        stats.meshShaderActive = activeRenderPath ==
            Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryMeshShader;
        stats.meshShaderFallbackReason = meshShaderFallbackReason;
        // SceneManager resolves stable handles before submission. Rendering
        // only consumes contiguous resource-table indices.
        for (const auto& drawInstance : packet.instances)
        {
            if (const MeshResource* mesh = sceneResources.mesh(drawInstance.meshId); mesh != nullptr)
            {
                stats.primitiveCount += mesh->indexCount / 3u;
                if (const auto* virtualAsset = sceneResources.virtualGeometry(drawInstance.meshId))
                {
                    stats.virtualDagNodeCount = static_cast<std::uint32_t>(virtualAsset->dagNodes.size());
                    stats.virtualSelectedNodeCount = 0u;
                    for (const auto& node : virtualAsset->dagNodes)
                        if (node.parentIndex == std::numeric_limits<std::uint32_t>::max())
                            ++stats.virtualSelectedNodeCount;
                }
            }
        }
        stats.taaHistoryValid = config.enableTaa && taaHistoryValid && hasRenderedFrame &&
                                packet.frameIndex == lastFrameIndex + 1u;
        if (!stats.taaHistoryValid)
        {
            // The compute pass receives a zero history weight whenever the
            // frame stream is discontinuous, after a resize, or after scene
            // mutation. This is a deterministic history state transition.
            taaHistoryValid = false;
        }
        deviceMemoryBytes = gpuAllocator.allocatedBytes();
        stats.deviceMemoryBytes = static_cast<std::uint64_t>(deviceMemoryBytes);
        const auto begin = std::chrono::steady_clock::now();
        int framebufferWidth = 0;
        int framebufferHeight = 0;
        if (window != nullptr)
        {
            glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
        }
        if (!initialized || device == VK_NULL_HANDLE || deviceLost)
        {
            if (deviceLost)
            {
                // Device loss is not recoverable for this renderer instance;
                // make the public state agree with the documented policy even
                // when the loss was observed while waiting for a resize.
                initialized = false;
                fatalError = true;
            }
            if (initialized && !deviceLost && swapchain == VK_NULL_HANDLE && framebufferWidth > 0 &&
                framebufferHeight > 0)
            {
                framebufferResized = true;
            }
            stats.minimized = framebufferWidth <= 0 || framebufferHeight <= 0;
            stats.deviceLost = deviceLost;
            stats.fatalError = fatalError;
            stats.cpuFrameMs = elapsedMilliseconds(begin);
            return stats;
        }

        // Acquiring from or presenting to a zero-sized GLFW framebuffer is
        // neither useful nor portable.  Preserve the old swapchain while the
        // window is minimized and recreate it after the first non-zero frame.
        if (framebufferWidth <= 0 || framebufferHeight <= 0)
        {
            framebufferResized = true;
            stats.minimized = true;
            stats.cpuFrameMs = elapsedMilliseconds(begin);
            return stats;
        }

        if (framebufferResized || swapchain == VK_NULL_HANDLE)
        {
            const VoidResult resizeResult = recreateSwapchain();
            if (!resizeResult)
            {
                setError(resizeResult.error().describe());
                stats.deviceLost = deviceLost;
                if (deviceLost)
                {
                    initialized = false;
                    fatalError = true;
                }
                stats.fatalError = fatalError;
                stats.cpuFrameMs = elapsedMilliseconds(begin);
                return stats;
            }
            lastError.clear();
            stats.recreatedSwapchain = !swapchainImageViews.empty();
            if (swapchain == VK_NULL_HANDLE || swapchainExtent.width == 0 ||
                swapchainExtent.height == 0)
            {
                stats.minimized = true;
                stats.cpuFrameMs = elapsedMilliseconds(begin);
                return stats;
            }
        }

        auto& frame = frames[currentFrame];
        VkResult result = frameContext.wait(device, frame);
        if (result != VK_SUCCESS)
        {
            setError(vkFailure("vkWaitForFences", result));
            if (result == VK_ERROR_DEVICE_LOST)
            {
                deviceLost = true;
            }
            fatalError = true;
            initialized = false;
            stats.deviceLost = deviceLost;
            stats.fatalError = true;
            stats.cpuFrameMs = elapsedMilliseconds(begin);
            return stats;
        }
        if (currentFrame < frameUploadBuffers.size())
        {
            for (auto& upload : frameUploadBuffers[currentFrame])
                gpuAllocator.destroy(upload);
            frameUploadBuffers[currentFrame].clear();
        }
        // Resources retired by the previous command buffers become safe to
        // destroy once the frame-slot fence has signalled.  Keep the provider
        // on the same serial as the frame scheduler so transient images and
        // views cannot be freed while still referenced by the GPU.
        if (renderSerial >= config.framesInFlight)
        {
            frameGraphProvider.collectCompleted(renderSerial - config.framesInFlight);
        }
        frameGraphProvider.beginFrame(renderSerial);
        if (timestampsEnabled && frame.submitted)
        {
            double gpuMilliseconds = -1.0;
            result = frameContext.readGpuTime(device, frame, gpuMilliseconds);
            if (result == VK_SUCCESS)
            {
                stats.gpuFrameMs = gpuMilliseconds;
            }
            for (std::uint32_t passIndex = 0;
                passIndex < frame.passNames.size() && passIndex < frameContext.maxPassCount;
                ++passIndex)
            {
                double passMilliseconds = -1.0;
                if (frameContext.readPassTime(device, frame, passIndex, passMilliseconds) ==
                    VK_SUCCESS)
                {
                    stats.gpuPasses.push_back({frame.passNames[passIndex], passMilliseconds});
                    const auto& name = frame.passNames[passIndex];
                    if (name == "Hi-Z build") {
                        stats.gpuHiZBuildMs = passMilliseconds;
                        if (config.enableTwoPhaseOcclusion)
                            stats.gpuTwoPhaseMs = passMilliseconds;
                    }
                }
            }
            double stageMilliseconds = -1.0;
            if (frameContext.readStageTime(device, frame, 0, stageMilliseconds) == VK_SUCCESS)
                stats.gpuFrustumCullMs = stageMilliseconds;
            if (frameContext.readStageTime(device, frame, 1, stageMilliseconds) == VK_SUCCESS)
                stats.gpuIndirectBuildMs = stageMilliseconds;
            if (frameContext.readStageTime(device, frame, 2, stageMilliseconds) == VK_SUCCESS)
                stats.gpuHiZBuildMs = stageMilliseconds;
            if (config.enableTwoPhaseOcclusion &&
                frameContext.readStageTime(device, frame, 3, stageMilliseconds) == VK_SUCCESS)
                stats.gpuTwoPhaseMs = stageMilliseconds;
        }
        if (frame.submitted &&
            currentFrame < clusterOverflowReadbacks.size())
        {
            const auto overflowBytes = gpuAllocator.readBuffer(
                clusterOverflowReadbacks[currentFrame], 0, sizeof(std::uint32_t));
            if (overflowBytes && overflowBytes.value().size() >= sizeof(std::uint32_t))
            {
                std::uint32_t overflow = 0;
                std::memcpy(&overflow, overflowBytes.value().data(), sizeof(overflow));
                stats.clusterOverflowCount = overflow;
            }
        }
        if (frame.submitted &&
            currentFrame < virtualGeometryReadbacks.size() &&
            currentFrame < virtualGeometryValid.size() && virtualGeometryValid[currentFrame])
        {
            const auto counters = gpuAllocator.readBuffer(
                virtualGeometryReadbacks[currentFrame], 0, sizeof(std::uint32_t) * 5u);
            if (counters && counters.value().size() >= sizeof(std::uint32_t) * 5u)
            {
                std::memcpy(&stats.virtualVisibleMeshletCount, counters.value().data(),
                    sizeof(std::uint32_t));
                std::memcpy(&stats.virtualIndirectCommandCount,
                    counters.value().data() + sizeof(std::uint32_t), sizeof(std::uint32_t));
                std::memcpy(&stats.virtualInvalidVisibilityCount,
                    counters.value().data() + sizeof(std::uint32_t) * 2u, sizeof(std::uint32_t));
                std::memcpy(&stats.virtualSelectedNodeCount,
                    counters.value().data() + sizeof(std::uint32_t) * 3u, sizeof(std::uint32_t));
                std::memcpy(&stats.virtualLodSwitchCount,
                    counters.value().data() + sizeof(std::uint32_t) * 4u, sizeof(std::uint32_t));
            }
        }
        if (frame.submitted && currentFrame < virtualPageRequestReadbacks.size() &&
            currentFrame < virtualPageRequestValid.size() &&
            virtualPageRequestValid[currentFrame])
        {
            const auto countBytes = gpuAllocator.readBuffer(
                virtualPageRequestReadbacks[currentFrame], 0u, sizeof(std::uint32_t));
            if (countBytes && countBytes->size() >= sizeof(std::uint32_t))
            {
                std::uint32_t rawCount = 0u;
                std::memcpy(&rawCount, countBytes->data(), sizeof(rawCount));
                const std::uint32_t pageCount = currentFrame <
                        virtualPageRequestPageCounts.size()
                    ? virtualPageRequestPageCounts[currentFrame] : 0u;
                const std::uint32_t capacity = std::min<std::uint32_t>(pageCount,
                    DebugReadbackManager::VirtualPageRequestCapacity);
                const std::uint32_t copiedCount = std::min(rawCount, capacity);
                stats.virtualPageRequestOverflowCount = rawCount > capacity
                    ? rawCount - capacity : 0u;
                if (copiedCount != 0u)
                {
                    const auto requestBytes = gpuAllocator.readBuffer(
                        virtualPageRequestReadbacks[currentFrame], sizeof(std::uint32_t),
                        static_cast<VkDeviceSize>(copiedCount) * sizeof(std::uint32_t));
                    if (requestBytes && requestBytes->size() >=
                            static_cast<std::size_t>(copiedCount) * sizeof(std::uint32_t))
                    {
                        std::vector<std::uint32_t> requests(copiedCount);
                        std::memcpy(requests.data(), requestBytes->data(),
                            requests.size() * sizeof(requests[0]));
                        const auto invalid = std::remove_if(requests.begin(), requests.end(),
                            [pageCount](std::uint32_t page) { return page >= pageCount; });
                        stats.virtualPageRequestOverflowCount +=
                            static_cast<std::uint32_t>(requests.end() - invalid);
                        requests.erase(invalid, requests.end());
                        std::sort(requests.begin(), requests.end());
                        requests.erase(std::unique(requests.begin(), requests.end()),
                            requests.end());
                        stats.virtualPageRequestCount =
                            static_cast<std::uint32_t>(requests.size());
                        const std::uint32_t meshId = currentFrame <
                                virtualPageRequestMeshIds.size()
                            ? virtualPageRequestMeshIds[currentFrame]
                            : std::numeric_limits<std::uint32_t>::max();
                        if (meshId != std::numeric_limits<std::uint32_t>::max())
                        {
                            if (auto* streamer = sceneResources.virtualGeometryStreamer(meshId))
                            {
                                // Requests are consumed two frames after GPU
                                // production. Preserve the GPU ordering while
                                // assigning a deterministic priority within
                                // this readback batch.
                                for (std::size_t i = 0u; i < requests.size(); ++i)
                                    (void)streamer->requestPage(requests[i],
                                        1000000.0f - static_cast<float>(i),
                                        packet.frameIndex);
                            }
                        }
                    }
                }
            }
            virtualPageRequestValid[currentFrame] = false;
        }
        if (frame.submitted && config.enableGpuDrivenScene &&
            currentFrame < gpuVisibilityReadbacks.size() &&
            currentFrame < gpuVisibilityValid.size() && gpuVisibilityValid[currentFrame])
        {
            gpuSceneBuffers.setFrameIndex(currentFrame);
            const auto counts = gpuAllocator.readBuffer(gpuVisibilityReadbacks[currentFrame], 0,
                sizeof(std::uint32_t) * VisibilityReadbackHeaderCount);
            if (counts && counts.value().size() >=
                    sizeof(std::uint32_t) * VisibilityReadbackHeaderCount)
            {
                std::uint32_t frustumCount = 0, firstPhase = 0, secondPhase = 0;
                std::uint32_t indirectCount = 0, phase2IndirectCount = 0;
                std::memcpy(&frustumCount, counts.value().data(), sizeof(frustumCount));
                std::memcpy(&firstPhase, counts.value().data() + sizeof(frustumCount), sizeof(firstPhase));
                std::memcpy(&secondPhase, counts.value().data() + sizeof(frustumCount) * 2u,
                    sizeof(secondPhase));
                std::memcpy(&indirectCount, counts.value().data() + sizeof(frustumCount) * 3u,
                    sizeof(indirectCount));
                std::memcpy(&phase2IndirectCount, counts.value().data() + sizeof(frustumCount) * 4u,
                    sizeof(phase2IndirectCount));
                stats.frustumVisibleInstanceCount = frustumCount;
                stats.occludedInstanceCount = config.enableTwoPhaseOcclusion
                    ? frustumCount >= firstPhase ? frustumCount - firstPhase : 0u : 0u;
                stats.visibleInstanceCount = firstPhase + (config.enableTwoPhaseOcclusion ? secondPhase : 0u);
                stats.indirectDrawCount = indirectCount +
                    (config.enableTwoPhaseOcclusion ? phase2IndirectCount : 0u);

                const auto& expected = currentFrame < gpuReferenceVisible.size()
                    ? gpuReferenceVisible[currentFrame] : std::vector<std::uint32_t>{};
                const std::uint32_t firstCount = std::min(firstPhase, VisibilityReadbackCapacity);
                const std::uint32_t secondCount = std::min(secondPhase, VisibilityReadbackCapacity);
                std::vector<bool> present(gpuSceneInstanceCount, false);
                const auto firstBytes = gpuAllocator.readBuffer(gpuVisibilityReadbacks[currentFrame],
                    sizeof(std::uint32_t) * VisibilityReadbackHeaderCount,
                    static_cast<VkDeviceSize>(firstCount) * sizeof(std::uint32_t));
                if (firstBytes)
                {
                    const auto* ids = reinterpret_cast<const std::uint32_t*>(firstBytes.value().data());
                    for (std::uint32_t i = 0; i < firstCount; ++i)
                    {
                        const auto slot = ids[i];
                        if (slot < present.size()) present[slot] = true;
                    }
                }
                if (config.enableTwoPhaseOcclusion)
                {
                    const auto secondBytes = gpuAllocator.readBuffer(gpuVisibilityReadbacks[currentFrame],
                        sizeof(std::uint32_t) *
                            (VisibilityReadbackHeaderCount + VisibilityReadbackCapacity),
                        static_cast<VkDeviceSize>(secondCount) * sizeof(std::uint32_t));
                    if (secondBytes)
                    {
                        const auto* ids = reinterpret_cast<const std::uint32_t*>(secondBytes.value().data());
                        for (std::uint32_t i = 0; i < secondCount; ++i)
                        {
                            const auto slot = ids[i];
                            if (slot < present.size()) present[slot] = true;
                        }
                    }
                }
                if (config.enableTwoPhaseOcclusion)
                {
                    // Hi-Z is allowed to remove genuinely occluded frustum
                    // candidates. Pixel-level reference-vs-occlusion checks
                    // are performed by the InstanceId comparison tool; the
                    // in-frame audit only reports structural readback health
                    // for this mode.
                    stats.gpuVisibilityMissingCount = 0;
                    stats.gpuVisibilityValidationPassed = true;
                }
                else
                {
                    for (const auto slot : expected)
                        if (slot >= present.size() || !present[slot])
                            ++stats.gpuVisibilityMissingCount;
                    stats.gpuVisibilityValidationPassed =
                        stats.gpuVisibilityMissingCount == 0;
                }
            }
        }
        if (frame.submitted && config.enableGpuDrivenScene &&
            currentFrame < instanceIdReadbacks.size() &&
            currentFrame < instanceIdReadbackValid.size() &&
            instanceIdReadbackValid[currentFrame])
        {
            const auto& readback = instanceIdReadbacks[currentFrame];
            const VkDeviceSize pixelBytes = sizeof(std::uint32_t);
            const VkDeviceSize pixelCount = readback.size / pixelBytes;
            if (pixelCount > 0)
            {
                const auto pixels = gpuAllocator.readBuffer(readback, 0,
                    pixelCount * pixelBytes);
                if (pixels)
                {
                    const auto* ids = reinterpret_cast<const std::uint32_t*>(pixels.value().data());
                    std::vector<std::uint32_t> presentIds;
                    if (!config.instanceIdReportPath.empty())
                        presentIds.reserve(static_cast<std::size_t>(pixelCount));
                    for (VkDeviceSize i = 0; i < pixelCount; ++i)
                    {
                        const std::uint32_t encoded = ids[i];
                        if (encoded != 0)
                        {
                            if (encoded - 1u >= gpuSceneInstanceCount)
                                ++stats.gpuInstanceIdInvalidPixelCount;
                            else if (!config.instanceIdReportPath.empty())
                                presentIds.push_back(encoded - 1u);
                        }
                    }
                    if (!config.instanceIdReportPath.empty())
                    {
                        std::sort(presentIds.begin(), presentIds.end());
                        presentIds.erase(std::unique(presentIds.begin(), presentIds.end()),
                            presentIds.end());
                        std::ofstream report(config.instanceIdReportPath, std::ios::app);
                        if (report)
                        {
                            report << instanceIdReadbackFrameIndices[currentFrame];
                            for (const std::uint32_t id : presentIds) report << ',' << id;
                            report << '\n';
                        }
                    }
                }
            }
            instanceIdReadbackValid[currentFrame] = false;
        }
        frame.submitted = false;

        // The frame timeline is also the lifetime source for GPU-scene slots,
        // even when a device uses the legacy material descriptor fallback.
        // Query it independently of bindless-table availability.
        {
            std::uint64_t completedTimeline = 0;
            if (vkGetSemaphoreCounterValue(
                    device, frameContext.timelineSemaphore, &completedTimeline) == VK_SUCCESS)
            {
                if (bindlessTable.initialized())
                    (void)bindlessTable.collect(completedTimeline);
                // GPU-scene slots follow the same submission timeline as
                // bindless descriptors. Reclaiming them here makes a
                // create/destroy-heavy scene safe without forcing a device
                // idle or leaking stable slots until shutdown.
                (void)gpuSceneState.collect(completedTimeline);
            }
        }

        result = swapchainState.acquire(frame.imageAvailable, stats.swapchainImageIndex);
        if (result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            framebufferResized = true;
            const VoidResult recreateResult = recreateSwapchain();
            if (!recreateResult)
            {
                setError(recreateResult.error().describe());
                if (deviceLost)
                {
                    initialized = false;
                    fatalError = true;
                }
            }
            else
            {
                lastError.clear();
            }
            stats.recreatedSwapchain = true;
            stats.deviceLost = deviceLost;
            stats.fatalError = fatalError;
            stats.cpuFrameMs = elapsedMilliseconds(begin);
            return stats;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        {
            setError(vkFailure("vkAcquireNextImageKHR", result));
            if (result == VK_ERROR_DEVICE_LOST)
            {
                deviceLost = true;
            }
            fatalError = true;
            initialized = false;
            stats.deviceLost = deviceLost;
            stats.fatalError = true;
            stats.cpuFrameMs = elapsedMilliseconds(begin);
            return stats;
        }
        stats.suboptimal = result == VK_SUBOPTIMAL_KHR;
        framebufferResized = framebufferResized || stats.suboptimal;
        if (stats.swapchainImageIndex >= presentReadySemaphores.size())
        {
            setError("Acquired swapchain image has no presentation semaphore");
            initialized = false;
            fatalError = true;
            stats.fatalError = true;
            stats.cpuFrameMs = elapsedMilliseconds(begin);
            return stats;
        }

        // Keep the fence signaled until command recording has succeeded.  If
        // a validation/runtime error occurs while resetting the command pool
        // or recording, the next frame cannot deadlock waiting on an
        // unsignaled fence that was never submitted.
        result = frameContext.resetCommandPool(device, frame);
        if (result != VK_SUCCESS)
        {
            setError(vkFailure("vkResetCommandPool", result));
            stats.deviceLost = result == VK_ERROR_DEVICE_LOST;
            deviceLost = deviceLost || stats.deviceLost;
            initialized = false;
            fatalError = true;
            stats.fatalError = true;
            stats.cpuFrameMs = elapsedMilliseconds(begin);
            return stats;
        }

        const std::filesystem::path screenshotPath =
            std::exchange(pendingScreenshotPath, std::filesystem::path{});
        BufferAllocation screenshotReadback{};
        if (!screenshotPath.empty())
        {
            VkBufferCreateInfo readbackInfo{};
            readbackInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            readbackInfo.size = static_cast<VkDeviceSize>(swapchainExtent.width) *
                                static_cast<VkDeviceSize>(swapchainExtent.height) * 4u;
            readbackInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            readbackInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            const auto allocation =
                gpuAllocator.createBuffer(readbackInfo, MemoryUsage::GpuToCpu);
            if (!allocation)
            {
                setError(allocation.error().describe());
                initialized = false;
                fatalError = true;
                stats.fatalError = true;
                stats.cpuFrameMs = elapsedMilliseconds(begin);
                return stats;
            }
            screenshotReadback = allocation.value();
        }

        // The debug InstanceId attachment is copied asynchronously from the
        // Present pass. Allocate it lazily per frame slot because its size is
        // swapchain-dependent and keep it alive until that slot's fence has
        // completed on a later frame.
        if (config.enableGpuDrivenScene &&
            currentFrame < instanceIdReadbacks.size())
        {
            const VkDeviceSize instanceIdBytes = static_cast<VkDeviceSize>(swapchainExtent.width) *
                static_cast<VkDeviceSize>(swapchainExtent.height) * sizeof(std::uint32_t);
            auto& allocation = instanceIdReadbacks[currentFrame];
            if (allocation.buffer == VK_NULL_HANDLE || allocation.size < instanceIdBytes)
            {
                if (allocation.buffer != VK_NULL_HANDLE) gpuAllocator.destroy(allocation);
                VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                info.size = instanceIdBytes;
                info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                const auto created = gpuAllocator.createBuffer(info, MemoryUsage::GpuToCpu);
                if (!created)
                {
                    gpuAllocator.destroy(screenshotReadback);
                    setError(created.error().describe());
                    initialized = false;
                    fatalError = true;
                    stats.fatalError = true;
                    stats.cpuFrameMs = elapsedMilliseconds(begin);
                    return stats;
                }
                allocation = created.value();
            }
            instanceIdReadbackValid[currentFrame] = false;
        }
        else if (currentFrame < instanceIdReadbackValid.size())
        {
            // Virtual Geometry has its own visibility ID attachments and does
            // not populate the legacy GBuffer InstanceId texture. Clear a
            // stale slot when switching paths so a later indexed frame cannot
            // consume an old readback as if it belonged to the current frame.
            instanceIdReadbackValid[currentFrame] = false;
        }

        if (currentFrame < virtualGeometryValid.size())
            virtualGeometryValid[currentFrame] = false;
        if (currentFrame < virtualPageRequestValid.size())
            virtualPageRequestValid[currentFrame] = false;
        const VoidResult recordResult = recordFrame(frame, stats.swapchainImageIndex, packet,
            screenshotReadback.buffer);
        if (!recordResult)
        {
            gpuAllocator.destroy(screenshotReadback);
            setError(recordResult.error().describe());
            stats.deviceLost = deviceLost;
            initialized = false;
            fatalError = true;
            stats.fatalError = true;
            stats.cpuFrameMs = elapsedMilliseconds(begin);
            return stats;
        }
        stats.executedPasses = frame.passNames;
        switch (activeRenderPath)
        {
        case Halcyon::Renderer::Scene::RenderPathMode::DeferredIndexed:
            stats.renderPath = "DeferredIndexed";
            break;
        case Halcyon::Renderer::Scene::RenderPathMode::GpuDrivenIndexed:
            stats.renderPath = "GpuDrivenIndexed";
            break;
        case Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryIndexed:
            stats.renderPath = "VirtualGeometryIndexed";
            break;
        case Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryMeshShader:
            stats.renderPath = "VirtualGeometryMeshShader";
            break;
        }
        stats.materialDescriptorBindCount = materialDescriptorBindCount;
        stats.gpuDrivenActive = gpuDrivenActive;
        stats.gpuFallbackInstanceCount = gpuFallbackInstanceCount;
        if (!config.enableGpuDrivenScene)
        {
            // The legacy path submits the extracted scene directly on the
            // CPU and has no GPU visibility result. Expose its submitted
            // instance count so stress CSVs remain useful for A/B analysis.
            stats.visibleInstanceCount = static_cast<std::uint32_t>(packet.instances.size());
            stats.frustumVisibleInstanceCount = stats.visibleInstanceCount;
            stats.indirectDrawCount = 0;
            stats.occludedInstanceCount = 0;
        }
        else if (!gpuDrivenActive)
        {
            stats.visibleInstanceCount = static_cast<std::uint32_t>(packet.instances.size());
            stats.indirectDrawCount = 0;
        }

        result = frameContext.resetFence(device, frame);
        if (result != VK_SUCCESS)
        {
            gpuAllocator.destroy(screenshotReadback);
            setError(vkFailure("vkResetFences", result));
            stats.deviceLost = result == VK_ERROR_DEVICE_LOST;
            deviceLost = deviceLost || stats.deviceLost;
            initialized = false;
            fatalError = true;
            stats.fatalError = true;
            stats.cpuFrameMs = elapsedMilliseconds(begin);
            return stats;
        }

        result = frameContext.submit(
            graphicsQueue, frame, presentReadySemaphores[stats.swapchainImageIndex]);
        if (result != VK_SUCCESS)
        {
            gpuAllocator.destroy(screenshotReadback);
            setError(vkFailure("vkQueueSubmit2", result));
            deviceLost = deviceLost || result == VK_ERROR_DEVICE_LOST;
            initialized = false;
            fatalError = true;
            stats.deviceLost = deviceLost;
            stats.fatalError = true;
            stats.cpuFrameMs = elapsedMilliseconds(begin);
            return stats;
        }
        ++renderSerial;
        swapchainImageInitialized[stats.swapchainImageIndex] = true;
        result = frameContext.present(presentQueue,
            swapchain,
            presentReadySemaphores[stats.swapchainImageIndex],
            stats.swapchainImageIndex);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        {
            framebufferResized = true;
            stats.suboptimal = true;
        }
        else if (result != VK_SUCCESS)
        {
            if (screenshotReadback.buffer != VK_NULL_HANDLE)
            {
                (void)frameContext.wait(device, frame);
                gpuAllocator.destroy(screenshotReadback);
            }
            setError(vkFailure("vkQueuePresentKHR", result));
            deviceLost = deviceLost || result == VK_ERROR_DEVICE_LOST;
            initialized = false;
            fatalError = true;
            stats.deviceLost = deviceLost;
            stats.fatalError = true;
            stats.cpuFrameMs = elapsedMilliseconds(begin);
            currentFrame = (currentFrame + 1) % static_cast<std::uint32_t>(frames.size());
            return stats;
        }

        if (screenshotReadback.buffer != VK_NULL_HANDLE)
        {
            result = frameContext.wait(device, frame);
            if (result != VK_SUCCESS)
            {
                gpuAllocator.destroy(screenshotReadback);
                setError(vkFailure("screenshot frame fence", result));
                deviceLost = deviceLost || result == VK_ERROR_DEVICE_LOST;
                initialized = false;
                fatalError = true;
                stats.deviceLost = deviceLost;
                stats.fatalError = true;
                stats.cpuFrameMs = elapsedMilliseconds(begin);
                return stats;
            }
            const VoidResult screenshotResult =
                writeScreenshot(screenshotReadback, screenshotPath);
            gpuAllocator.destroy(screenshotReadback);
            if (!screenshotResult)
            {
                setError(screenshotResult.error().describe());
                initialized = false;
                fatalError = true;
                stats.fatalError = true;
                stats.cpuFrameMs = elapsedMilliseconds(begin);
                return stats;
            }
            stats.screenshotWritten = true;
        }

        currentFrame = (currentFrame + 1) % static_cast<std::uint32_t>(frames.size());
        stats.rendered = true;
        // A completed frame also establishes valid persistent Hi-Z history;
        // this is independent of whether temporal AA is enabled.
        hasRenderedFrame = true;
        // This field tracks renderer-owned VMA allocations, not the physical
        // heap capacity reported in Capabilities.
        deviceMemoryBytes = gpuAllocator.allocatedBytes();
        stats.deviceMemoryBytes = static_cast<std::uint64_t>(deviceMemoryBytes);
        stats.quality.rayQueryEnabled = rayQueryEnabled;
        stats.taaHistoryValid = config.enableTaa && taaHistoryValid && hasRenderedFrame &&
                                packet.frameIndex == lastFrameIndex + 1u;
        if (config.enableTaa)
        {
            taaHistoryValid = true;
            lastFrameIndex = packet.frameIndex;
        }
        previousInstances.assign(packet.instances.begin(), packet.instances.end());
        previousViewProjection = packet.camera.viewProjection;
        previousPacketValid = true;
        stats.cpuFrameMs = elapsedMilliseconds(begin);
        HALCYON_PROFILE_FRAME();
        return stats;
    }

    static double elapsedMilliseconds(const std::chrono::steady_clock::time_point& begin) noexcept
    {
        const auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(now - begin).count();
    }
};

VoidResult Renderer::Impl::recordFrame(
    FrameContext& frame, std::uint32_t imageIndex, const FramePacket& packet,
    VkBuffer screenshotReadback)
{
    HALCYON_PROFILE_SCOPE("Renderer::recordFrame");
    materialDescriptorBindCount = 0;
    // These are per-recording-frame facts. Clear them before any early
    // validation failure so counters from the previous render cannot be
    // reported as belonging to a failed VirtualGeometry frame.
    virtualGeometryActive = false;
    virtualVisibilityValid = false;
    if (imageIndex >= swapchainImages.size() || imageIndex >= swapchainImageViews.size())
    {
        return fail("Acquired swapchain image index is out of range");
    }
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkResult result = vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);
    if (result != VK_SUCCESS)
    {
        deviceLost = deviceLost || result == VK_ERROR_DEVICE_LOST;
        return fail(vkFailure("vkBeginCommandBuffer", result));
    }
    if (timestampsEnabled)
    {
        vkCmdResetQueryPool(frame.commandBuffer, frameContext.timestampPool, frame.queryBase,
            2u + frameContext.maxPassCount * 2u + VulkanFrameContext::StageQueryCount);
        vkCmdWriteTimestamp2(frame.commandBuffer, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            frameContext.timestampPool, frame.queryBase);
    }
    if (currentFrame >= frameUploadBuffers.size())
        return fail("GPU scene upload frame slot is out of range");

    RendererConfig passConfig = config;
    if (config.renderPath == Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryIndexed ||
        config.renderPath == Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryMeshShader)
    {
        constexpr std::uint32_t unsupportedFlags =
            static_cast<std::uint32_t>(Halcyon::Renderer::Scene::Ecs::RenderableFlags::Transparent) |
            static_cast<std::uint32_t>(Halcyon::Renderer::Scene::Ecs::RenderableFlags::DoubleSided) |
            static_cast<std::uint32_t>(Halcyon::Renderer::Scene::Ecs::RenderableFlags::AlphaMasked) |
            Halcyon::Renderer::Scene::kGpuSceneCpuFallbackFlag;
        // M5 V1 supports multiple instances that share one virtual mesh table.
        // Mixed assets need separate index-buffer streams, so the whole packet
        // falls back instead of silently dropping incompatible draws.
        const auto* firstVirtualAsset = packet.instances.empty() ? nullptr :
            sceneResources.virtualGeometryDense(packet.instances.front().meshId);
        const std::uint32_t sharedVirtualMeshId = packet.instances.empty()
            ? std::numeric_limits<std::uint32_t>::max() : packet.instances.front().meshId;
        const auto fitsStorageRange = [&](std::size_t count, std::size_t elementSize)
        {
            return count <= std::numeric_limits<VkDeviceSize>::max() / elementSize &&
                static_cast<VkDeviceSize>(count * elementSize) <=
                    physicalProperties.limits.maxStorageBufferRange;
        };
        const auto paddedByteCount = [](std::size_t count) noexcept
        {
            return count > std::numeric_limits<std::size_t>::max() - 3u
                ? std::numeric_limits<std::size_t>::max()
                : (count + 3u) & ~std::size_t(3u);
        };
        const std::uint64_t possibleMeshletDraws = firstVirtualAsset == nullptr
            ? 0u
            : static_cast<std::uint64_t>(firstVirtualAsset->meshlets.size()) *
                static_cast<std::uint64_t>(packet.instances.size());
        const bool meshShaderPath = config.renderPath ==
            Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryMeshShader;
        const std::uint64_t backendMeshletCapacity = meshShaderPath
            ? static_cast<std::uint64_t>(caps.maxMeshWorkGroupCountX)
            : static_cast<std::uint64_t>(physicalProperties.limits.maxDrawIndirectCount);
        const bool allVirtualAssets = !packet.instances.empty() &&
            packet.instances.size() <= VulkanFrameResources::MaxVirtualGeometryInstances &&
            firstVirtualAsset != nullptr &&
            firstVirtualAsset->meshlets.size() <=
                VulkanFrameResources::MaxVirtualGeometryMeshlets &&
            firstVirtualAsset->vertices.size() <= std::numeric_limits<std::uint32_t>::max() &&
            firstVirtualAsset->meshletVertices.size() <= std::numeric_limits<std::uint32_t>::max() &&
            firstVirtualAsset->meshletTriangles.size() <= std::numeric_limits<std::uint32_t>::max() &&
            firstVirtualAsset->indices.size() <= std::numeric_limits<std::uint32_t>::max() &&
            firstVirtualAsset->lods.size() <= std::numeric_limits<std::uint32_t>::max() &&
            firstVirtualAsset->dagNodes.size() <=
                VulkanFrameResources::MaxVirtualGeometryMeshlets &&
            fitsStorageRange(firstVirtualAsset->meshlets.size(),
                sizeof(VulkanSceneResources::VirtualGeometryGpuMeshlet)) &&
            fitsStorageRange(firstVirtualAsset->vertices.size(),
                sizeof(Halcyon::Renderer::Scene::StaticSceneVertex)) &&
            fitsStorageRange(firstVirtualAsset->meshletVertices.size(), sizeof(std::uint32_t)) &&
            fitsStorageRange(paddedByteCount(firstVirtualAsset->meshletTriangles.size()),
                sizeof(std::uint8_t)) &&
            fitsStorageRange(firstVirtualAsset->indices.size(), sizeof(std::uint32_t)) &&
            fitsStorageRange(firstVirtualAsset->lods.size(), sizeof(Halcyon::Renderer::Scene::VirtualGeometryLod)) &&
            fitsStorageRange(firstVirtualAsset->dagNodes.size(),
                sizeof(VulkanSceneResources::VirtualGeometryGpuDagNode)) &&
            fitsStorageRange(firstVirtualAsset->dagNodes.size(), sizeof(std::uint32_t) * 4u) &&
            possibleMeshletDraws <= VulkanFrameResources::MaxVirtualGeometryMeshlets &&
            possibleMeshletDraws <= backendMeshletCapacity &&
            std::all_of(packet.instances.begin(), packet.instances.end(),
                [&](const InstanceData& instance)
                {
                    const auto* asset = sceneResources.virtualGeometryDense(instance.meshId);
                    const auto* gpu = sceneResources.virtualGeometryBuffersDense(instance.meshId);
                    const bool finiteTransform = std::all_of(instance.transform.begin(),
                        instance.transform.end(), [](float value) { return std::isfinite(value); });
                    const glm::mat4 model = glm::make_mat4(instance.transform.data());
                    const float determinant = glm::determinant(glm::mat3(model));
                    return (instance.flags & unsupportedFlags) == 0 && asset != nullptr &&
                        instance.meshId == sharedVirtualMeshId && asset == firstVirtualAsset &&
                        asset->hasUniformPrimitiveMaterial() &&
                        sceneResources.virtualGeometryMaterialCompatible(instance.materialId) &&
                        instance.materialId <=
                            Halcyon::Renderer::Scene::kVirtualVisibilityMaterialIndexMask &&
                        gpu != nullptr && !asset->meshlets.empty() && finiteTransform &&
                        // The visibility pipeline culls back faces with a
                        // fixed winding. Mirrored transforms would invert
                        // that winding, so keep them on the established
                        // indexed fallback path for correctness.
                        std::isfinite(determinant) && determinant > 1.0e-12f &&
                        gpu->meshlets.buffer != VK_NULL_HANDLE &&
                        gpu->meshletVertices.buffer != VK_NULL_HANDLE &&
                        gpu->vertices.buffer != VK_NULL_HANDLE &&
                        gpu->indices.buffer != VK_NULL_HANDLE;
                });
        if (!allVirtualAssets)
        {
            std::string fallbackReason =
                "Virtual Geometry packet is incompatible with the selected backend";
            if (packet.instances.empty())
                fallbackReason = "Virtual Geometry packet has no instances";
            else if (firstVirtualAsset == nullptr)
                fallbackReason = "Virtual Geometry sidecar/runtime asset is unavailable";
            else if (possibleMeshletDraws > VulkanFrameResources::MaxVirtualGeometryMeshlets ||
                possibleMeshletDraws > backendMeshletCapacity)
                fallbackReason = "Virtual Geometry meshlet command capacity is insufficient";
            else
            {
                for (const auto& instance : packet.instances)
                {
                    const auto* asset = sceneResources.virtualGeometryDense(instance.meshId);
                    const auto* gpu = sceneResources.virtualGeometryBuffersDense(instance.meshId);
                    if ((instance.flags & unsupportedFlags) != 0u)
                        fallbackReason = "Virtual Geometry instance uses unsupported material/render flags";
                    else if (instance.meshId != sharedVirtualMeshId || asset != firstVirtualAsset)
                        fallbackReason = "Virtual Geometry packet references more than one mesh table";
                    else if (asset == nullptr || !asset->hasUniformPrimitiveMaterial() ||
                        !sceneResources.virtualGeometryMaterialCompatible(instance.materialId))
                        fallbackReason = "Virtual Geometry material ABI is incompatible";
                    else if (gpu == nullptr || gpu->meshlets.buffer == VK_NULL_HANDLE ||
                        gpu->meshletVertices.buffer == VK_NULL_HANDLE ||
                        gpu->vertices.buffer == VK_NULL_HANDLE || gpu->indices.buffer == VK_NULL_HANDLE)
                        fallbackReason = "Virtual Geometry GPU buffers are unavailable";
                    else
                    {
                        const glm::mat4 model = glm::make_mat4(instance.transform.data());
                        const float determinant = glm::determinant(glm::mat3(model));
                        if (!std::isfinite(determinant) || determinant <= 1.0e-12f)
                            fallbackReason = "Virtual Geometry instance transform is singular or mirrored";
                        else
                            continue;
                    }
                    break;
                }
            }
            passConfig.renderPath = config.enableGpuDrivenScene
                ? Halcyon::Renderer::Scene::RenderPathMode::GpuDrivenIndexed
                : Halcyon::Renderer::Scene::RenderPathMode::DeferredIndexed;
            fallbackReason += "; using the established indexed fallback";
            if (meshShaderPath && meshShaderFallbackReason != fallbackReason)
            {
                meshShaderFallbackReason = fallbackReason;
                HALCYON_LOG_WARN(meshShaderFallbackReason);
            }
            setError(std::move(fallbackReason));
        }
    }
    activeRenderPath = passConfig.renderPath ==
            Halcyon::Renderer::Scene::RenderPathMode::DeferredIndexed &&
            config.enableGpuDrivenScene
        ? Halcyon::Renderer::Scene::RenderPathMode::GpuDrivenIndexed
        : passConfig.renderPath;
    virtualGeometryActive = passConfig.renderPath ==
            Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryIndexed ||
        passConfig.renderPath ==
            Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryMeshShader;
    virtualVisibilityValid = false;
    if (passConfig.renderPath !=
            Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryIndexed &&
        passConfig.renderPath !=
            Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryMeshShader)
        virtualHiZInitialized = false;
    const auto sceneUpload = gpuSceneBuffers.recordPendingUploads(
        frame.commandBuffer, gpuAllocator, frameUploadBuffers[currentFrame]);
    if (!sceneUpload) return sceneUpload;
    gpuSceneBuffers.setFrameIndex(currentFrame);

    struct ImportedTarget
    {
        VkImageView view = VK_NULL_HANDLE;
    } importedTarget{swapchainImageViews[imageIndex]};

    Graph::FrameGraph graph;
    graph.setResourceProvider(&frameGraphProvider);
    if (currentFrame < frameDescriptorPools.size())
    {
        frameDescriptorPool = frameDescriptorPools[currentFrame];
        (void)vkResetDescriptorPool(device, frameDescriptorPool, 0);
    }

    constexpr std::uint32_t gpuUnsupportedFlags =
        static_cast<std::uint32_t>(Halcyon::Renderer::Scene::Ecs::RenderableFlags::Transparent) |
        static_cast<std::uint32_t>(Halcyon::Renderer::Scene::Ecs::RenderableFlags::DoubleSided) |
        Halcyon::Renderer::Scene::kGpuSceneCpuFallbackFlag;
    VkDescriptorSet gpuCullSet = VK_NULL_HANDLE;
    VkDescriptorSet gpuIndirectSet = VK_NULL_HANDLE;
    VkDescriptorSet gpuGraphicsSet = VK_NULL_HANDLE;
    VkDescriptorSet gpuPhase2GraphicsSet = VK_NULL_HANDLE;
    std::uint32_t gpuMaterialId = 0;
    const VkBuffer gpuVertexBuffer = sceneResources.gpuDrivenVertexBuffer();
    const VkBuffer gpuIndexBuffer = sceneResources.gpuDrivenIndexBuffer();
    const VkBuffer gpuMeshDrawBuffer = sceneResources.meshDrawBuffer();
    const bool legacyGpuDrivenPath = passConfig.renderPath !=
            Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryIndexed &&
        passConfig.renderPath !=
            Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryMeshShader;
    bool gpuIndirectCompatible = legacyGpuDrivenPath && config.enableGpuDrivenScene &&
        gpuSceneInstanceCount != 0 &&
        gpuVertexBuffer != VK_NULL_HANDLE && gpuIndexBuffer != VK_NULL_HANDLE &&
        gpuMeshDrawBuffer != VK_NULL_HANDLE;
    std::uint32_t cpuFallbackInstanceCount = 0;
    if (gpuIndirectCompatible && !packet.instances.empty())
    {
        gpuMaterialId = packet.instances.front().materialId;
        cpuFallbackInstanceCount = static_cast<std::uint32_t>(packet.instances.size());
    }
    gpuDrivenActive = gpuIndirectCompatible;
    gpuFallbackInstanceCount = cpuFallbackInstanceCount;

    FramePassContext ctx{};
    ctx.device = device;
    ctx.cmdDrawMeshTasksIndirectCount = cmdDrawMeshTasksIndirectCount;
    ctx.frame = &frame;
    ctx.packet = &packet;
    ctx.config = &passConfig;
    ctx.pipelines = &pipelines;
    ctx.gpuSceneBuffers = &gpuSceneBuffers;
    ctx.sceneResources = &sceneResources;
    ctx.frameGraphProvider = &frameGraphProvider;
    ctx.bindlessTable = &bindlessTable;
    ctx.frameRecorder = &frameRecorder;
    ctx.frameResources = &frameResources;
    ctx.frameContext = &frameContext;
    ctx.debugReadbacks = &debugReadbacks;
    ctx.gpuAllocator = &gpuAllocator;
    ctx.descriptorPool = frameDescriptorPool;
    ctx.linearSampler = linearSampler;
    ctx.swapchainExtent = swapchainExtent;
    ctx.swapchainFormat = swapchainFormat;
    ctx.depthFormat = depthFormat;
    ctx.width = swapchainExtent.width;
    ctx.height = swapchainExtent.height;
    ctx.currentFrame = currentFrame;
    ctx.imageIndex = imageIndex;
    ctx.virtualIndirectDrawCapacity = std::min<std::uint32_t>(
        VulkanFrameResources::MaxVirtualGeometryMeshlets,
        physicalProperties.limits.maxDrawIndirectCount);
    ctx.virtualMeshWorkGroupCapacity = caps.maxMeshWorkGroupCountX;
    // GpuDrivenIndexed is also selected by enableGpuDrivenScene while the
    // public renderPath remains DeferredIndexed. Keep the bindless ABI for
    // that established path; when GPU-driven rendering is disabled (including
    // a VirtualGeometry-to-Deferred fallback), do not leak the optional M5
    // pipeline capability into legacy CPU draw state.
    ctx.gpuDrivenBindless = legacyGpuDrivenPath && config.enableGpuDrivenScene
        ? gpuDrivenBindless : false;
    ctx.timestampsEnabled = timestampsEnabled;
    ctx.previousPacketValid = previousPacketValid;
    ctx.hasRenderedFrame = hasRenderedFrame;
    ctx.taaHistoryValid = taaHistoryValid;
    ctx.previousViewProjection = previousViewProjection;
    ctx.cpuDraw.sceneResources = &sceneResources;
    ctx.cpuDraw.pipelines = &pipelines;
    ctx.cpuDraw.materialDescriptorBindCount = &materialDescriptorBindCount;
    ctx.cpuDraw.previousInstances = &previousInstances;
    ctx.cpuDraw.previousPacketValid = previousPacketValid;
    ctx.cpuDraw.previousViewProjection = previousViewProjection;
    ctx.cpuDraw.gpuDrivenBindless = ctx.gpuDrivenBindless;
    ctx.gpuSceneInstanceCount = gpuSceneInstanceCount;
    ctx.gpuMaterialCount = gpuMaterialCount;
    ctx.gpuMaterialId = gpuMaterialId;
    ctx.gpuIndirectCompatible = gpuIndirectCompatible;
    ctx.gpuVertexBuffer = gpuVertexBuffer;
    ctx.gpuIndexBuffer = gpuIndexBuffer;
    ctx.gpuMeshDrawBuffer = gpuMeshDrawBuffer;
    ctx.screenshotReadback = screenshotReadback;
    ctx.swapchainView = swapchainImageViews[imageIndex];
    ctx.swapchainImages = &swapchainImages;
    ctx.swapchainImageInitialized = &swapchainImageInitialized;
    ctx.frameUploadBuffers = &frameUploadBuffers;
    ctx.iblInitialized = &iblInitialized;
    ctx.iblImageInitialized = &iblImageInitialized;
    ctx.virtualHiZInitialized = &virtualHiZInitialized;
    ctx.virtualHiZImageInitialized = &virtualHiZImageInitialized;
    ctx.virtualVisibilityValid = &virtualVisibilityValid;
    ctx.taaHistoryFlip = &taaHistoryFlip;
    ctx.taaHistoryInitializedA = &taaHistoryInitializedA;
    ctx.taaHistoryInitializedB = &taaHistoryInitializedB;
    ctx.fatalError = &fatalError;
    ctx.deviceLost = &deviceLost;
    ctx.lastError = &lastError;

    // Virtual Geometry owns its meshlet cull and indirect stream. Do not
    // record the legacy GPU-driven culling commands when both features are
    // enabled; the virtual path still uses the uploaded GPU scene material
    // table and transforms, but has no consumer for these command buffers.
    if (passConfig.renderPath !=
        Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryIndexed)
    {
        const auto cullResult = recordGpuDrivenCulling(ctx);
        if (!cullResult) return cullResult;
        gpuIndirectCompatible = ctx.gpuIndirectCompatible;
        gpuCullSet = ctx.gpuCullSet;
        gpuIndirectSet = ctx.gpuIndirectSet;
        gpuGraphicsSet = ctx.gpuGraphicsSet;
        gpuPhase2GraphicsSet = ctx.gpuPhase2GraphicsSet;
    }

    const std::uint32_t width = swapchainExtent.width;
    const std::uint32_t height = swapchainExtent.height;
    if (passConfig.renderPath !=
            Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryIndexed &&
        config.enableGpuDrivenScene && currentFrame < gpuReferenceVisible.size())
    {
        const glm::mat4& vp = packet.camera.viewProjection;
        const glm::vec4 rows[4] = {
            {vp[0][0], vp[1][0], vp[2][0], vp[3][0]},
            {vp[0][1], vp[1][1], vp[2][1], vp[3][1]},
            {vp[0][2], vp[1][2], vp[2][2], vp[3][2]},
            {vp[0][3], vp[1][3], vp[2][3], vp[3][3]}};
        std::array<glm::vec4, 6> planes = {
            rows[3] + rows[0], rows[3] - rows[0], rows[3] + rows[1],
            rows[3] - rows[1], rows[3] + rows[2], rows[3] - rows[2]};
        for (auto& plane : planes)
        {
            const float length = glm::length(glm::vec3(plane));
            if (length > 1.0e-6f) plane /= length;
        }
        auto& reference = gpuReferenceVisible[currentFrame];
        reference.clear();
        const auto& bounds = gpuSceneState.soa().bounds;
        const auto& meshMaterials = gpuSceneState.soa().meshMaterials;
        const std::uint32_t count = std::min<std::uint32_t>(gpuSceneInstanceCount,
            static_cast<std::uint32_t>(bounds.size()));
        for (std::uint32_t slot = 0; slot < count; ++slot)
        {
            if (slot >= meshMaterials.size()) continue;
            const auto& material = meshMaterials[slot];
            if ((material.flags & gpuUnsupportedFlags) != 0u ||
                (!gpuDrivenBindless && material.materialIndex != gpuMaterialId))
                continue;
            const auto& sphere = bounds[slot].sphereCenterRadius;
            const glm::vec4 value{sphere[0], sphere[1], sphere[2], sphere[3]};
            if (Halcyon::Renderer::Scene::sphereInsideFrustum(planes, value))
                reference.push_back(slot);
        }
    }

    auto declaredResources = frameResources.declare(
        graph, static_cast<std::uint32_t>(packet.lights.size()));
    if (!declaredResources)
    {
        return VoidResult::failure(declaredResources.error());
    }
    auto m3 = declaredResources.value();
    ctx.shadow = m3.csm;
    ctx.gbuffer0 = m3.gbuffer0;
    ctx.gbuffer1 = m3.gbuffer1;
    ctx.gbuffer2 = m3.gbuffer2;
    ctx.motion = m3.motion;
    ctx.instanceId = m3.instanceId;
    ctx.depth = m3.depth;
    ctx.hiz = m3.hiz;
    ctx.hdr = m3.hdr;
    ctx.historyA = m3.historyA;
    ctx.historyB = m3.historyB;
    ctx.irradiance = m3.irradiance;
    ctx.prefiltered = m3.prefiltered;
    ctx.brdfLut = m3.brdfLut;
    ctx.visibility = m3.visibility;
    ctx.visibilityPrimitive = m3.visibilityPrimitive;
    ctx.visibilityBarycentrics = m3.visibilityBarycentrics;
    ctx.materialClassification = m3.materialClassification;
    ctx.virtualTransforms = m3.virtualTransforms;
    ctx.virtualMeshMaterials = m3.virtualMeshMaterials;
    ctx.virtualCullFrame = m3.virtualCullFrame;
    ctx.visibleMeshlets = m3.visibleMeshlets;
    ctx.visibleMeshletCount = m3.visibleMeshletCount;
    ctx.selectedLodNodes = m3.selectedLodNodes;
    ctx.selectedLodCount = m3.selectedLodCount;
    ctx.lodBalanceDepth = m3.lodBalanceDepth;
    ctx.virtualPageRequests = m3.virtualPageRequests;
    ctx.virtualPageRequestCount = m3.virtualPageRequestCount;
    ctx.meshletIndirect = m3.meshletIndirect;
    ctx.meshletIndirectCount = m3.meshletIndirectCount;
    ctx.meshletMeshIndirect = m3.meshletMeshIndirect;
    ctx.meshletMeshIndirectCount = m3.meshletMeshIndirectCount;
    ctx.virtualValidation = m3.virtualValidation;
    // M5 visibility resources remain declared for ABI stability; unsupported
    // devices continue through the established deferred/GPU-driven passes.
    ctx.clusterRanges = m3.clusterRanges;
    ctx.clusterIndices = m3.clusterIndices;
    ctx.clusterOverflow = m3.clusterOverflow;
    ctx.lightBuffer = m3.lights;
    ctx.clusterCamera = m3.clusterCamera;
    ctx.shadowConstants = m3.shadowConstants;
    ctx.tileCount = m3.clusterCount;

    Graph::FrameGraphRenderPass::ImportDescriptor swapImport{};
    swapImport.attachments = Graph::FrameGraphAttachmentFlags::Color0;
    swapImport.viewport.width = width;
    swapImport.viewport.height = height;
    swapImport.clearFlags = Graph::FrameGraphAttachmentFlags::Color0;
    ctx.output = graph.import("Swapchain", swapImport, {&importedTarget});

    computeCascadeMatrices(packet, ctx.cascadeMatrices, ctx.cascadeSplits);

    VkImageMemoryBarrier2 swapchainBeginBarrier{};
    swapchainBeginBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    swapchainBeginBarrier.srcStageMask = swapchainImageInitialized[imageIndex]
                                             ? VK_PIPELINE_STAGE_2_NONE
                                             : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    swapchainBeginBarrier.srcAccessMask = VK_ACCESS_2_NONE;
    swapchainBeginBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    swapchainBeginBarrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    swapchainBeginBarrier.oldLayout = swapchainImageInitialized[imageIndex]
                                          ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                          : VK_IMAGE_LAYOUT_UNDEFINED;
    swapchainBeginBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    swapchainBeginBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    swapchainBeginBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    swapchainBeginBarrier.image = swapchainImages[imageIndex];
    swapchainBeginBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    swapchainBeginBarrier.subresourceRange.levelCount = 1;
    swapchainBeginBarrier.subresourceRange.layerCount = 1;
    VkDependencyInfo swapchainBeginDependency{};
    swapchainBeginDependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    swapchainBeginDependency.imageMemoryBarrierCount = 1;
    swapchainBeginDependency.pImageMemoryBarriers = &swapchainBeginBarrier;
    vkCmdPipelineBarrier2(frame.commandBuffer, &swapchainBeginDependency);

    const bool virtualGeometryPath =
        passConfig.renderPath == Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryIndexed ||
        passConfig.renderPath == Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryMeshShader;
    if (!virtualGeometryPath)
        addCsmShadowPasses(graph, ctx);
    addVirtualGeometryPasses(graph, ctx);
    if (!virtualGeometryPath)
        addGBufferPass(graph, ctx);
    if (!virtualGeometryPath)
    {
        addHiZOcclusionPass(graph, ctx);
        addClusterBuildPass(graph, ctx);
    }
    if (!virtualGeometryPath)
        addDeferredLightingPass(graph, ctx);
    if (!virtualGeometryPath)
        addTransparencyPass(graph, ctx);
    // M5 writes HDR and camera motion directly, then participates in the same
    // temporal resolve, tonemap and present chain as indexed rendering.
    addTaaResolvePass(graph, ctx);
    addTonemapPass(graph, ctx);
    addPresentPass(graph, ctx);

    graph.compile(Graph::CompileOptions{false});
    if (!graph.compileResult())
    {
        return fail(graph.lastError().message, Halcyon::ErrorCode::InvalidState);
    }
    frame.passNames.clear();
    for (const auto handle : graph.compileResult().executionOrder)
    {
        if (const auto* pass = graph.compileResult().pass(handle); pass != nullptr)
            frame.passNames.push_back(pass->name);
    }
    Graph::CommandContext commands;
    graph.execute(commands,
        Graph::ExecuteOptions{
            &frame,
            [this, &frame](const Graph::PassExecutionContext& context)
            {
                (void)frameContext.writePassTimestamp(frame.commandBuffer, frame,
                    context.executionIndex, true);
            },
            [this, &frame](const Graph::PassExecutionContext& context)
            {
                (void)frameContext.writePassTimestamp(frame.commandBuffer, frame,
                    context.executionIndex, false);
            }});
    if (graph.lastError())
    {
        return fail(graph.lastError().message, Halcyon::ErrorCode::InvalidState);
    }
    if (fatalError)
    {
        return fail(lastError.empty() ? "Vulkan pass recording failed" : lastError,
            Halcyon::ErrorCode::Backend);
    }
    ctx.gpuIndirectCompatible = gpuIndirectCompatible;
    recordVisibilityReadback(ctx);
    if (timestampsEnabled)
    {
        vkCmdWriteTimestamp2(frame.commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
            frameContext.timestampPool, frame.queryBase + 1);
    }
    taaHistoryFlip = !taaHistoryFlip;
    result = vkEndCommandBuffer(frame.commandBuffer);
    if (result != VK_SUCCESS)
    {
        deviceLost = deviceLost || result == VK_ERROR_DEVICE_LOST;
        return fail(vkFailure("vkEndCommandBuffer", result));
    }
    return ok();
}


Renderer::Renderer() noexcept
        : impl_(new (std::nothrow) Impl{})
{
}

Renderer::~Renderer()
{
    if (impl_ != nullptr)
    {
        impl_->cleanup();
        delete impl_;
        impl_ = nullptr;
    }
}

Renderer::Renderer(Renderer&& other) noexcept
        : impl_(other.impl_)
{
    other.impl_ = nullptr;
}

Renderer& Renderer::operator=(Renderer&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }
    if (impl_ != nullptr)
    {
        impl_->cleanup();
        delete impl_;
    }
    impl_ = other.impl_;
    other.impl_ = nullptr;
    return *this;
}

Halcyon::Result<void> Renderer::initialize(GLFWwindow* window, const RendererConfig& config)
{
    if (impl_ == nullptr)
    {
        return Halcyon::Result<void>::failure(Halcyon::Error{
            Halcyon::ErrorCode::OutOfMemory, "failed to allocate Vulkan renderer state"});
    }
    impl_->cleanup();
    impl_->lastError.clear();
    if (window == nullptr)
    {
        impl_->setError("Renderer::initialize received a null GLFWwindow");
        return Halcyon::Result<void>::failure(Halcyon::Error{Halcyon::ErrorCode::InvalidArgument,
            impl_->lastError,
            "Vulkan renderer initialization"});
    }
    impl_->config = config;
    impl_->window = window;
    impl_->requestedExtent = {config.initialExtent.width, config.initialExtent.height};
    if (impl_->config.targetFrameTimeMs <= 0.0f)
    {
        impl_->config.targetFrameTimeMs = 16.667f;
    }
    try
    {
        VoidResult result = impl_->deviceState.initialize(window, impl_->config);
        if (!result)
        {
            impl_->setError(result.error().describe());
            impl_->cleanup();
            return result;
        }
        result = impl_->createFrameDescriptors();
        if (!result)
        {
            impl_->setError(result.error().describe());
            impl_->cleanup();
            return result;
        }
        result =
            impl_->gpuAllocator.initialize(impl_->instance, impl_->physicalDevice, impl_->device);
        if (!result)
        {
            impl_->setError(result.error().describe());
            impl_->cleanup();
            return result;
        }
        result = impl_->frameGraphProvider.initialize(
            impl_->device, impl_->physicalDevice, impl_->gpuAllocator);
        if (!result)
        {
            impl_->setError(result.error().describe());
            impl_->cleanup();
            return result;
        }
        result = impl_->gpuSceneBuffers.initialize(impl_->device, impl_->gpuAllocator,
            131072u, impl_->config.framesInFlight);
        if (!result)
        {
            impl_->setError(result.error().describe());
            impl_->cleanup();
            return result;
        }
#if HALCYON_BUILD_FRAMEGRAPH
        if (impl_->caps.descriptorIndexing)
        {
            const auto bindlessResult = impl_->bindlessTable.initialize(
                impl_->device, bindlessConfig(impl_->physicalProperties.limits));
            impl_->caps.bindlessTable = static_cast<bool>(bindlessResult);
        }
#endif
        // M5 is additive: the visibility/material ABI needs descriptor
        // indexing, indirect-count draws, and fragment barycentrics. Keep the
        // requested scene live on the established indexed GPU path when any
        // one of those capabilities is absent.
        VkFormatProperties visibilityFormatProperties{};
        vkGetPhysicalDeviceFormatProperties(impl_->physicalDevice, VK_FORMAT_R32_UINT,
            &visibilityFormatProperties);
        const bool visibilityIntegerAttachment =
            (visibilityFormatProperties.optimalTilingFeatures &
                (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) ==
            (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
             VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
        VkFormatProperties hdrFormatProperties{};
        vkGetPhysicalDeviceFormatProperties(impl_->physicalDevice,
            VK_FORMAT_R32G32B32A32_SFLOAT, &hdrFormatProperties);
        constexpr VkFormatFeatureFlags virtualOutputFeatures =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
            VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        const bool hdrStorageImage =
            (hdrFormatProperties.optimalTilingFeatures & virtualOutputFeatures) ==
            virtualOutputFeatures;
        VkFormatProperties motionFormatProperties{};
        vkGetPhysicalDeviceFormatProperties(impl_->physicalDevice,
            VK_FORMAT_R16G16_SFLOAT, &motionFormatProperties);
        const bool motionStorageImage =
            (motionFormatProperties.optimalTilingFeatures & virtualOutputFeatures) ==
            virtualOutputFeatures;
        VkFormatProperties hizFormatProperties{};
        vkGetPhysicalDeviceFormatProperties(impl_->physicalDevice,
            VK_FORMAT_R32_SFLOAT, &hizFormatProperties);
        const auto hizFeatures = hizFormatProperties.optimalTilingFeatures;
        const bool hizImageSupport =
            (hizFeatures & (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) ==
            (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
        const auto& descriptorLimits = impl_->physicalProperties.limits;
        const bool virtualDescriptorSupport =
            descriptorLimits.maxPerStageDescriptorStorageBuffers >= 7u &&
            descriptorLimits.maxPerStageDescriptorSampledImages >= 6u &&
            descriptorLimits.maxPerStageDescriptorStorageImages >= 2u &&
            descriptorLimits.maxPerStageDescriptorSamplers >= 1u &&
            descriptorLimits.maxDescriptorSetStorageBuffers >= 7u &&
            descriptorLimits.maxDescriptorSetSampledImages >= 6u &&
            descriptorLimits.maxDescriptorSetStorageImages >= 2u &&
            descriptorLimits.maxDescriptorSetSamplers >= 1u &&
            descriptorLimits.maxPerStageResources >= 16u &&
            descriptorLimits.maxColorAttachments >= 3u &&
            descriptorLimits.maxFragmentOutputAttachments >= 3u &&
            descriptorLimits.maxPushConstantsSize >= 128u;
        // The mesh shader pipeline is optional. Auto mode falls back before
        // swapchain creation when the extension/feature cannot be enabled;
        // Required mode reports a startup error.
        if (impl_->config.renderPath == Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryMeshShader)
        {
            const bool meshAvailable = impl_->caps.meshShader &&
                impl_->config.meshShader != FeatureMode::Disabled;
            if (!meshAvailable && impl_->config.meshShader == FeatureMode::Required)
            {
                impl_->setError("VirtualGeometryMeshShader is required but VK_EXT_mesh_shader "
                    "or its meshShader feature is unavailable");
                impl_->cleanup();
                return Halcyon::Result<void>::failure({Halcyon::ErrorCode::Unsupported,
                    impl_->lastError, "Vulkan renderer initialization"});
            }
            if (!meshAvailable)
            {
                impl_->config.renderPath =
                    Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryIndexed;
                impl_->meshShaderFallbackReason =
                    "VK_EXT_mesh_shader or its meshShader feature is unavailable";
                impl_->setError("VirtualGeometryMeshShader unavailable; fell back to "
                    "VirtualGeometryIndexed");
            }
        }
        if (impl_->config.renderPath == Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryIndexed &&
            (!impl_->caps.descriptorIndexing || !impl_->caps.indirectCount ||
                !impl_->caps.scalarBlockLayout || !impl_->caps.geometryShader ||
                !impl_->caps.fragmentBarycentric || !visibilityIntegerAttachment ||
                !hdrStorageImage || !motionStorageImage || !hizImageSupport ||
                !virtualDescriptorSupport))
        {
            impl_->config.renderPath = impl_->config.enableGpuDrivenScene
                ? Halcyon::Renderer::Scene::RenderPathMode::GpuDrivenIndexed
                : Halcyon::Renderer::Scene::RenderPathMode::DeferredIndexed;
            impl_->setError(impl_->config.enableGpuDrivenScene
                ? "VirtualGeometryIndexed unavailable; fell back to GpuDrivenIndexed "
                  "(descriptor limits/indexing, indirect count, barycentrics, integer attachment, Hi-Z format, or HDR/motion storage missing)"
                : "VirtualGeometryIndexed unavailable; fell back to DeferredIndexed "
                  "(descriptor limits/indexing, indirect count, barycentrics, integer attachment, Hi-Z format, or HDR/motion storage missing)");
        }
        impl_->swapchainState.enableVsync = impl_->config.enableVsync;
        result = impl_->swapchainState.initialize(impl_->physicalDevice,
            impl_->device,
            impl_->surface,
            window,
            impl_->graphicsQueueFamily,
            impl_->presentQueueFamily,
            VkExtent2D{impl_->requestedExtent.width, impl_->requestedExtent.height});
        if (!result)
        {
            impl_->setError(result.error().describe());
            impl_->cleanup();
            return result;
        }
        result = impl_->createTimelineSemaphore();
        if (!result)
        {
            impl_->setError(result.error().describe());
            impl_->cleanup();
            return result;
        }
        result = impl_->createFrameResources();
        if (!result)
        {
            impl_->setError(result.error().describe());
            impl_->cleanup();
            return result;
        }
        result = impl_->sceneResources.initialize(impl_->device,
            impl_->physicalDevice,
            impl_->frames.front().commandPool,
            impl_->graphicsQueue,
            impl_->gpuAllocator,
            impl_->gpuUploader,
            impl_->config.enableGpuDrivenScene);
        if (!result)
        {
            impl_->setError(result.error().describe());
            impl_->cleanup();
            return result;
        }
        int framebufferWidth = 0;
        int framebufferHeight = 0;
        if (window != nullptr)
        {
            glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
        }
        if (framebufferWidth > 0 && framebufferHeight > 0)
        {
            impl_->requestedExtent = {static_cast<std::uint32_t>(framebufferWidth),
                static_cast<std::uint32_t>(framebufferHeight)};
        }
        result = impl_->createSwapchain();
        if (!result)
        {
            impl_->setError(result.error().describe());
            impl_->cleanup();
            return result;
        }
        impl_->initialized = true;
        impl_->framebufferResized = false;
        return Halcyon::Result<void>::success();
    }
    catch (const std::exception& exception)
    {
        impl_->setError(exception.what());
        impl_->cleanup();
        return Halcyon::Result<void>::failure(Halcyon::Error{
            Halcyon::ErrorCode::Backend, impl_->lastError, "Vulkan renderer initialization"});
    }
    catch (...)
    {
        impl_->setError("unknown exception during Vulkan renderer initialization");
        impl_->cleanup();
        return Halcyon::Result<void>::failure(Halcyon::Error{
            Halcyon::ErrorCode::Backend, impl_->lastError, "Vulkan renderer initialization"});
    }
}

FrameStats Renderer::render(const FramePacket& packet)
{
    if (impl_ == nullptr)
    {
        FrameStats stats{};
        stats.deviceLost = true;
        stats.fatalError = true;
        return stats;
    }
    return impl_->render(packet);
}

Halcyon::Result<void> Renderer::resize(Extent2D extent)
{
    if (impl_ == nullptr)
    {
        return Halcyon::Result<void>::failure(
            Halcyon::Error{Halcyon::ErrorCode::InvalidState, "renderer state is not allocated"});
    }
    impl_->requestedExtent = {extent.width, extent.height};
    impl_->framebufferResized = true;
    // Swapchain recreation is deliberately deferred to render().  GLFW can
    // invoke resize callbacks while the framebuffer is transiently zero-sized
    // or while the platform is still processing its window event.
    return Halcyon::Result<void>::success();
}

void Renderer::shutdown() noexcept
{
    if (impl_ != nullptr)
    {
        impl_->cleanup();
    }
}

const Capabilities& Renderer::capabilities() const noexcept
{
    static const Capabilities empty{};
    return impl_ != nullptr ? impl_->caps : empty;
}

const std::string& Renderer::lastError() const noexcept
{
    static const std::string empty;
    return impl_ != nullptr ? impl_->lastError : empty;
}

bool Renderer::initialized() const noexcept
{
    return impl_ != nullptr && impl_->initialized;
}

Halcyon::Result<void> Renderer::uploadSceneAsset(
    const Halcyon::Renderer::Scene::SceneDatabase& database,
    const Halcyon::Renderer::Scene::SceneImportResult& imported)
{
    if (impl_ == nullptr || !impl_->initialized)
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::InvalidState, "renderer is not initialized"});
    }
    const VkResult idle = vkDeviceWaitIdle(impl_->device);
    if (idle != VK_SUCCESS)
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::Backend, "failed to synchronize scene resource upload"});
    }
    auto result = impl_->sceneResources.uploadAsset(database, imported);
    if (result)
    {
        const auto bindless = impl_->synchronizeBindlessMaterials();
        if (!bindless) result = bindless;
    }
    impl_->deviceMemoryBytes = impl_->gpuAllocator.allocatedBytes();
    return result;
}

Halcyon::Result<void> Renderer::releaseSceneAsset(
    const Halcyon::Renderer::Scene::SceneImportResult& imported)
{
    if (impl_ == nullptr || !impl_->initialized)
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::InvalidState, "renderer is not initialized"});
    }
    const VkResult idle = vkDeviceWaitIdle(impl_->device);
    if (idle != VK_SUCCESS)
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::Backend, "failed to synchronize scene resource release"});
    }
    auto result = impl_->sceneResources.releaseAsset(imported);
    if (result)
    {
        const auto bindless = impl_->synchronizeBindlessMaterials();
        if (!bindless) result = bindless;
    }
    impl_->deviceMemoryBytes = impl_->gpuAllocator.allocatedBytes();
    return result;
}

Halcyon::Result<void> Renderer::remapFramePacket(
    Halcyon::Renderer::Scene::OwnedFramePacket& packet) const
{
    if (impl_ == nullptr || !impl_->initialized)
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::InvalidState, "renderer is not initialized"});
    }
    for (auto& instance : packet.instances)
    {
        const std::uint32_t stableMesh = instance.meshId;
        const std::uint32_t stableMaterial = instance.materialId;
        const auto mesh = impl_->sceneResources.meshDenseIndex(stableMesh);
        const auto material = impl_->sceneResources.materialDenseIndex(stableMaterial);
        if (mesh == std::numeric_limits<std::uint32_t>::max() ||
            material == std::numeric_limits<std::uint32_t>::max())
        {
            return Halcyon::Result<void>::failure({Halcyon::ErrorCode::InvalidState,
                "frame packet references an unmapped scene resource (mesh slot " +
                    std::to_string(stableMesh) + ", material slot " +
                    std::to_string(stableMaterial) + ")"});
        }
        instance.meshId = mesh;
        instance.materialId = material;
    }
    return Halcyon::Result<void>::success();
}

Halcyon::Result<void> Renderer::updateGpuScene(
    std::span<const InstanceData> instances)
{
    if (impl_ == nullptr || !impl_->initialized || impl_->frames.empty())
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::InvalidState, "renderer is not initialized"});
    // Avoid touching device-local memory for unchanged frames. This is the
    // temporary bridge until RenderExtractor deltas are uploaded by range.
    std::uint64_t hash = 1469598103934665603ull;
    for (const InstanceData& instance : instances)
    {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&instance);
        for (std::size_t i = 0; i < sizeof(InstanceData); ++i)
            hash = (hash ^ bytes[i]) * 1099511628211ull;
    }
    if (hash == impl_->gpuSceneContentHash)
        return Halcyon::Result<void>::success();
    Halcyon::Renderer::Scene::GpuSceneSoA scene;
    scene.transforms.resize(instances.size());
    scene.bounds.resize(instances.size());
    scene.meshMaterials.resize(instances.size());
    for (std::size_t i = 0; i < instances.size(); ++i)
    {
        scene.transforms[i].model = instances[i].transform;
        const glm::mat4 model = glm::make_mat4(instances[i].transform.data());
        const MeshResource* mesh = impl_->sceneResources.mesh(instances[i].meshId);
        if (mesh == nullptr)
            return Halcyon::Result<void>::failure({Halcyon::ErrorCode::InvalidState,
                "GPU scene references an unmapped mesh"});
        scene.bounds[i] = Halcyon::Renderer::Scene::computeWorldBounds(
            mesh->boundsMin, mesh->boundsMax, model);
        const std::uint32_t cpuFallback = mesh->indexCount == 0
            ? Halcyon::Renderer::Scene::kGpuSceneCpuFallbackFlag : 0u;
        scene.meshMaterials[i] = {instances[i].meshId, instances[i].materialId,
            instances[i].flags | cpuFallback, 0};
    }
    const std::uint32_t requiredCapacity = static_cast<std::uint32_t>(std::max({
        instances.size(),
        impl_->bindlessMaterialRows.size(),
        static_cast<std::size_t>(impl_->sceneResources.meshDrawCount())}));
    auto result = impl_->gpuSceneBuffers.ensureCapacity(requiredCapacity);
    if (!result) return result;
    impl_->gpuSceneInstanceCount = static_cast<std::uint32_t>(instances.size());
    if (!impl_->bindlessMaterialRows.empty())
    {
        result = impl_->gpuSceneBuffers.uploadMaterials(impl_->bindlessMaterialRows);
        if (!result) return result;
    }
    auto upload = impl_->gpuSceneBuffers.upload(scene);
    if (upload) impl_->gpuSceneContentHash = hash;
    return upload;
}

Halcyon::Result<void> Renderer::updateGpuSceneDelta(
    const Halcyon::Renderer::Scene::Ecs::RenderExtractor::Delta& delta)
{
    if (impl_ == nullptr || !impl_->initialized || impl_->frames.empty())
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::InvalidState, "renderer is not initialized"});
    const auto remap = [&](InstanceData instance) -> Halcyon::Result<InstanceData>
    {
        const auto mesh = impl_->sceneResources.meshDenseIndex(instance.meshId);
        const auto material = impl_->sceneResources.materialDenseIndex(instance.materialId);
        if (mesh == std::numeric_limits<std::uint32_t>::max() ||
            material == std::numeric_limits<std::uint32_t>::max())
            return Halcyon::Result<InstanceData>::failure({Halcyon::ErrorCode::InvalidState,
                "GPU scene delta references an unmapped resource"});
        instance.meshId = mesh;
        instance.materialId = material;
        return Halcyon::Result<InstanceData>::success(instance);
    };
    const auto boundsFor = [&](const InstanceData& instance)
    {
        const MeshResource* mesh = impl_->sceneResources.mesh(instance.meshId);
        if (mesh == nullptr)
            return Halcyon::Result<Halcyon::Renderer::Scene::BoundsRow>::failure(
                {Halcyon::ErrorCode::InvalidState, "GPU scene references an unmapped mesh"});
        const glm::mat4 model = glm::make_mat4(instance.transform.data());
        return Halcyon::Result<Halcyon::Renderer::Scene::BoundsRow>::success(
            Halcyon::Renderer::Scene::computeWorldBounds(
                mesh->boundsMin, mesh->boundsMax, model));
    };
    for (const auto& item : delta.created)
    {
        auto mapped = remap(item.instance);
        if (!mapped) return mapped.error();
        auto bounds = boundsFor(mapped.value());
        if (!bounds) return bounds.error();
        if (const MeshResource* mesh = impl_->sceneResources.mesh(mapped.value().meshId);
            mesh != nullptr && mesh->indexCount == 0)
        {
            mapped.value().flags |= Halcyon::Renderer::Scene::kGpuSceneCpuFallbackFlag;
        }
        if (!impl_->gpuSceneState.applyCreated(item.entity, mapped.value(), bounds.value()))
            return Halcyon::Result<void>::failure({Halcyon::ErrorCode::Backend,
                "failed to apply GPU scene create delta"});
    }
    for (const auto& item : delta.updated)
    {
        auto mapped = remap(item.instance);
        if (!mapped) return mapped.error();
        auto bounds = boundsFor(mapped.value());
        if (!bounds) return bounds.error();
        if (const MeshResource* mesh = impl_->sceneResources.mesh(mapped.value().meshId);
            mesh != nullptr && mesh->indexCount == 0)
        {
            mapped.value().flags |= Halcyon::Renderer::Scene::kGpuSceneCpuFallbackFlag;
        }
        if (!impl_->gpuSceneState.applyUpdated(item.entity, mapped.value(), bounds.value()) &&
            !impl_->gpuSceneState.applyCreated(item.entity, mapped.value(), bounds.value()))
            return Halcyon::Result<void>::failure({Halcyon::ErrorCode::Backend,
                "failed to apply GPU scene update delta"});
    }
    for (const auto entity : delta.destroyed)
    {
        (void)impl_->gpuSceneState.applyDestroyed(entity,
            impl_->renderSerial + impl_->frames.size());
    }
    const auto& dirty = impl_->gpuSceneState.dirtyRanges();
    if (dirty.empty()) return Halcyon::Result<void>::success();
    std::uint32_t required = 0;
    for (const auto& range : dirty)
        required = std::max(required, range.first + range.count);
    impl_->gpuSceneInstanceCount = std::max(impl_->gpuSceneInstanceCount, required);
    required = std::max({required,
        static_cast<std::uint32_t>(impl_->bindlessMaterialRows.size()),
        impl_->sceneResources.meshDrawCount()});
    const bool requiresFullUpload = required > impl_->gpuSceneBuffers.capacity();
    auto result = impl_->gpuSceneBuffers.ensureCapacity(required);
    if (!result) return result;
    if (!impl_->bindlessMaterialRows.empty())
    {
        result = impl_->gpuSceneBuffers.uploadMaterials(impl_->bindlessMaterialRows);
        if (!result) return result;
    }
    result = requiresFullUpload
        ? impl_->gpuSceneBuffers.upload(impl_->gpuSceneState.soa())
        : impl_->gpuSceneBuffers.uploadDirty(impl_->gpuSceneState.soa(), dirty);
    if (result) impl_->gpuSceneState.clearDirtyRanges();
    return result;
}

bool Renderer::gpuDrivenSceneEnabled() const noexcept
{
    return impl_ != nullptr && impl_->initialized && impl_->config.enableGpuDrivenScene;
}

bool Renderer::gpuDrivenBindlessEnabled() const noexcept
{
    // The bindless GPU-driven path only needs a CPU packet for fallback and
    // transparent draws. Virtual Geometry still consumes the complete,
    // densely-remapped instance list for meshlet culling, visibility IDs and
    // attribute reconstruction, even though it shares the bindless material
    // table with GPU-driven rendering.
    return impl_ != nullptr && impl_->initialized && impl_->gpuDrivenBindless &&
        impl_->config.renderPath !=
            Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryIndexed;
}

void Renderer::invalidateTaaHistory() noexcept
{
    if (impl_ != nullptr)
    {
        impl_->taaHistoryValid = false;
        impl_->hasRenderedFrame = false;
        impl_->previousPacketValid = false;
        impl_->previousInstances.clear();
        impl_->virtualHiZInitialized = false;
        impl_->virtualVisibilityValid = false;
    }
}

Halcyon::Result<void> Renderer::captureScreenshot(const std::filesystem::path& path)
{
    if (impl_ == nullptr)
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::InvalidState, "renderer state is not allocated"});
    }
    return impl_->captureScreenshot(path);
}

RendererNativeHandles Renderer::nativeHandles() const noexcept
{
    RendererNativeHandles handles{};
    if (impl_ == nullptr)
    {
        return handles;
    }
    handles.instance = impl_->instance;
    handles.physicalDevice = impl_->physicalDevice;
    handles.device = impl_->device;
    handles.graphicsQueue = impl_->graphicsQueue;
    handles.presentQueue = impl_->presentQueue;
    handles.swapchainFormat = impl_->swapchainFormat;
    handles.depthFormat = impl_->depthFormat;
    handles.swapchainExtent = impl_->swapchainExtent;
    handles.swapchainImageCount = static_cast<std::uint32_t>(impl_->swapchainImages.size());
    return handles;
}

void Renderer::setOverlayCallback(OverlayCallback callback) noexcept
{
    if (impl_ != nullptr)
    {
        impl_->overlayCallback = callback;
    }
}

} // namespace Halcyon::Vulkan
