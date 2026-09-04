#include "HalcyonVulkanRenderer.h"

#include "Core/Profiler.h"
#include "GpuAllocator.h"
#include "GpuUploader.h"
#include "VulkanBindlessTable.h"
#include "VulkanCommon.h"
#include "VulkanDemoResources.h"
#include "VulkanDevice.h"
#include "VulkanFrameContext.h"
#include "VulkanPipeline.h"
#include "VulkanSwapchain.h"
#include "../Quality/ClusteredLighting.h"

#ifndef HALCYON_BUILD_EXPERIMENTAL_M2
#define HALCYON_BUILD_EXPERIMENTAL_M2 0
#endif

#if HALCYON_BUILD_EXPERIMENTAL_M2
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
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#undef STB_IMAGE_WRITE_IMPLEMENTATION
#include <filesystem>
#include <glm/gtc/type_ptr.hpp>
#include <new>
#include <utility>
#include <vector>

namespace Halcyon::Vulkan
{
namespace Quality = Halcyon::Renderer::Quality;
#if HALCYON_BUILD_EXPERIMENTAL_M2
namespace Graph = Halcyon::Renderer::Graph;
#endif
namespace
{

#if HALCYON_BUILD_EXPERIMENTAL_M2
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
        32u, limits.maxPerStageDescriptorSampledImages, limits.maxDescriptorSetSampledImages);
    config.storageImageCapacity = descriptorCapacity(
        8u, limits.maxPerStageDescriptorStorageImages, limits.maxDescriptorSetStorageImages);
    config.uniformBufferCapacity = descriptorCapacity(
        12u, limits.maxPerStageDescriptorUniformBuffers, limits.maxDescriptorSetUniformBuffers);
    config.storageBufferCapacity = descriptorCapacity(
        8u, limits.maxPerStageDescriptorStorageBuffers, limits.maxDescriptorSetStorageBuffers);
    config.samplerCapacity = descriptorCapacity(
        16u, limits.maxPerStageDescriptorSamplers, limits.maxDescriptorSetSamplers);
    return config;
}

[[nodiscard]] VkPipelineStageFlags2 toVkStages(Graph::PipelineStage stages) noexcept
{
    VkPipelineStageFlags2 result = VK_PIPELINE_STAGE_2_NONE;
    if (Graph::any(stages & Graph::PipelineStage::VertexInput)) result |= VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
    if (Graph::any(stages & Graph::PipelineStage::VertexShader)) result |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    if (Graph::any(stages & Graph::PipelineStage::FragmentShader)) result |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    if (Graph::any(stages & Graph::PipelineStage::ComputeShader)) result |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    if (Graph::any(stages & Graph::PipelineStage::ColorOutput)) result |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (Graph::any(stages & Graph::PipelineStage::DepthTest)) result |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    if (Graph::any(stages & Graph::PipelineStage::Transfer)) result |= VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    if (Graph::any(stages & Graph::PipelineStage::DrawIndirect)) result |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    if (Graph::any(stages & Graph::PipelineStage::Host)) result |= VK_PIPELINE_STAGE_2_HOST_BIT;
    if (Graph::any(stages & Graph::PipelineStage::AllCommands)) result |= VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    return result;
}

[[nodiscard]] VkAccessFlags2 toVkAccess(Graph::AccessFlags access) noexcept
{
    VkAccessFlags2 result = VK_ACCESS_2_NONE;
    if (Graph::any(access & Graph::AccessFlags::VertexRead)) result |= VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
    if (Graph::any(access & Graph::AccessFlags::IndexRead)) result |= VK_ACCESS_2_INDEX_READ_BIT;
    if (Graph::any(access & Graph::AccessFlags::UniformRead)) result |= VK_ACCESS_2_UNIFORM_READ_BIT;
    if (Graph::any(access & Graph::AccessFlags::ShaderSampledRead)) result |= VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    if (Graph::any(access & Graph::AccessFlags::ShaderStorageRead)) result |= VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    if (Graph::any(access & Graph::AccessFlags::ShaderStorageWrite)) result |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    if (Graph::any(access & Graph::AccessFlags::IndirectRead)) result |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    if (Graph::any(access & Graph::AccessFlags::ColorWrite)) result |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    if (Graph::any(access & Graph::AccessFlags::DepthRead)) result |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    if (Graph::any(access & Graph::AccessFlags::DepthWrite)) result |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    if (Graph::any(access & Graph::AccessFlags::TransferRead)) result |= VK_ACCESS_2_TRANSFER_READ_BIT;
    if (Graph::any(access & Graph::AccessFlags::TransferWrite)) result |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
    if (Graph::any(access & Graph::AccessFlags::HostWrite)) result |= VK_ACCESS_2_HOST_WRITE_BIT;
    if (Graph::any(access & Graph::AccessFlags::ColorRead)) result |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
    if (Graph::any(access & Graph::AccessFlags::PresentRead)) result |= VK_ACCESS_2_NONE;
    return result;
}

[[nodiscard]] VkImageLayout toVkLayout(Graph::ImageLayout layout) noexcept
{
    switch (layout)
    {
    case Graph::ImageLayout::Undefined: return VK_IMAGE_LAYOUT_UNDEFINED;
    case Graph::ImageLayout::General: return VK_IMAGE_LAYOUT_GENERAL;
    case Graph::ImageLayout::ShaderReadOnly: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case Graph::ImageLayout::ColorAttachment: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case Graph::ImageLayout::DepthAttachment: return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    case Graph::ImageLayout::TransferSource: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case Graph::ImageLayout::TransferDestination: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    case Graph::ImageLayout::Present: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }
    return VK_IMAGE_LAYOUT_GENERAL;
}
#endif

} // namespace

struct Renderer::Impl
{
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
    VkImage& depthImage = swapchainState.depthImage;
    ImageAllocation& depthAllocation = swapchainState.depthAllocation;
    VkImageView& depthImageView = swapchainState.depthImageView;
    VkDeviceSize& depthMemorySize = swapchainState.depthMemorySize;
    VkFormat& depthFormat = swapchainState.depthFormat;
    bool& depthImageInitialized = swapchainState.depthImageInitialized;
    std::vector<VkImage>& swapchainImages = swapchainState.swapchainImages;
    std::vector<VkImageView>& swapchainImageViews = swapchainState.swapchainImageViews;
    std::vector<VkSemaphore>& presentReadySemaphores = swapchainState.presentReadySemaphores;
    std::vector<bool>& swapchainImageInitialized = swapchainState.swapchainImageInitialized;

    VulkanFrameContext frameContext;
    std::vector<FrameContext>& frames = frameContext.frames;
    std::uint32_t& currentFrame = frameContext.currentFrame;
    bool& timestampsEnabled = frameContext.timestampsEnabled;

    VulkanPipeline graphicsPipeline;

    VkExtent2D& requestedExtent = swapchainState.requestedExtent;
    bool& framebufferResized = swapchainState.framebufferResized;
    bool initialized = false;
    bool deviceLost = false;
    bool fatalError = false;
    bool& rayQueryEnabled = deviceState.rayQueryEnabled;
    VkDeviceSize deviceMemoryBytes = 0;
    GpuAllocator gpuAllocator;
    GpuUploader gpuUploader;
    VulkanDemoResources demoResources;
    VulkanBindlessTable bindlessTable;
    OverlayCallback overlayCallback = nullptr;
    std::uint32_t lastPresentedImage = 0;
    bool taaHistoryValid = false;
    bool hasRenderedFrame = false;
    std::uint64_t lastFrameIndex = 0;

    ~Impl()
    {
        cleanup();
    }

    void setError(std::string message)
    {
        lastError = std::move(message);
    }

    void cleanupSwapchain() noexcept
    {
        graphicsPipeline.destroy();
        swapchainState.cleanup();
    }

    void cleanup() noexcept
    {
        if (device != VK_NULL_HANDLE)
        {
            // Waiting is best effort during error cleanup; every child object
            // is still destroyed even when the device has already been lost.
            (void)vkDeviceWaitIdle(device);
            cleanupSwapchain();
            demoResources.cleanup();
            bindlessTable.shutdown();
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
        lastPresentedImage = 0;
        taaHistoryValid = false;
        hasRenderedFrame = false;
        lastFrameIndex = 0;
    }

    [[nodiscard]] VoidResult createTimelineSemaphore()
    {
        return frameContext.createTimeline(device);
    }

    [[nodiscard]] VoidResult createFrameResources()
    {
        const VoidResult result = frameContext.createResources(
            device, physicalDevice, physicalProperties, graphicsQueueFamily, config.framesInFlight);
        return result;
    }

    [[nodiscard]] VoidResult createSwapchain()
    {
        const VoidResult result = swapchainState.create();
        if (!result)
        {
            return result;
        }
        graphicsPipeline.destroy();
        const VoidResult pipelineResult = createGraphicsPipeline();
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
        graphicsPipeline.destroy();
        const VoidResult pipelineResult = createGraphicsPipeline();
        deviceMemoryBytes = gpuAllocator.allocatedBytes();
        // A swapchain resize changes the sampling footprint, so any temporal
        // history must be discarded before the next rendered frame.
        taaHistoryValid = false;
        hasRenderedFrame = false;
        return pipelineResult;
    }

    [[nodiscard]] VoidResult createGraphicsPipeline()
    {
        const auto result = graphicsPipeline.create(device,
            swapchainFormat,
            depthFormat,
            swapchainExtent,
            demoResources.textureSetLayout(),
            bindlessTable.initialized() ? bindlessTable.layout() : VK_NULL_HANDLE,
            demoResources.textured());
        // A textured pipeline can be unavailable (for example when its
        // generated SPIR-V is missing); VulkanPipeline falls back to the
        // embedded triangle shaders and reports the active mode.
        return result;
    }

    [[nodiscard]] VoidResult recordFrame(
        FrameContext& frame, std::uint32_t imageIndex, const FramePacket& packet);

    [[nodiscard]] VoidResult captureScreenshot(const std::filesystem::path& path)
    {
        if (!initialized || device == VK_NULL_HANDLE || swapchain == VK_NULL_HANDLE ||
            swapchainExtent.width == 0 || swapchainExtent.height == 0 ||
            lastPresentedImage >= swapchainImages.size())
        {
            return fail("No rendered frame is available for screenshot", Halcyon::ErrorCode::InvalidState);
        }
        (void)vkDeviceWaitIdle(device);
        const VkDeviceSize size = static_cast<VkDeviceSize>(swapchainExtent.width) *
                                  static_cast<VkDeviceSize>(swapchainExtent.height) * 4u;
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        auto bufferResult = gpuAllocator.createBuffer(info, MemoryUsage::GpuToCpu);
        if (!bufferResult) return bufferResult.error();
        const BufferAllocation readback = bufferResult.value();
        VkCommandBufferAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = frames.front().commandPool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkResult result = vkAllocateCommandBuffers(device, &alloc, &command);
        if (result == VK_SUCCESS)
        {
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            result = vkBeginCommandBuffer(command, &begin);
        }
        VkImageMemoryBarrier2 toCopy{};
        toCopy.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        toCopy.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        toCopy.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        toCopy.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        toCopy.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toCopy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toCopy.image = swapchainImages[lastPresentedImage];
        toCopy.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &toCopy;
        if (result == VK_SUCCESS) vkCmdPipelineBarrier2(command, &dep);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {swapchainExtent.width, swapchainExtent.height, 1};
        if (result == VK_SUCCESS) vkCmdCopyImageToBuffer(command, swapchainImages[lastPresentedImage],
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
        VkImageMemoryBarrier2 back = toCopy;
        back.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        back.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        back.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
        back.dstAccessMask = VK_ACCESS_2_NONE;
        back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        back.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        dep.pImageMemoryBarriers = &back;
        if (result == VK_SUCCESS) vkCmdPipelineBarrier2(command, &dep);
        if (result == VK_SUCCESS) result = vkEndCommandBuffer(command);
        if (result == VK_SUCCESS)
        {
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &command;
            result = vkQueueSubmit(graphicsQueue, 1, &submit, VK_NULL_HANDLE);
            if (result == VK_SUCCESS) result = vkQueueWaitIdle(graphicsQueue);
        }
        if (command != VK_NULL_HANDLE) vkFreeCommandBuffers(device, frames.front().commandPool, 1, &command);
        if (result != VK_SUCCESS)
        {
            gpuAllocator.destroy(readback);
            return fail(vkFailure("screenshot readback", result));
        }
        auto bytes = gpuAllocator.readBuffer(readback, 0, size);
        gpuAllocator.destroy(readback);
        if (!bytes) return bytes.error();
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(size));
        const auto* src = reinterpret_cast<const std::uint8_t*>(bytes.value().data());
        for (std::uint32_t y = 0; y < swapchainExtent.height; ++y)
        {
            for (std::uint32_t x = 0; x < swapchainExtent.width; ++x)
            {
                const std::size_t index = (static_cast<std::size_t>(y) * swapchainExtent.width + x) * 4u;
                const bool bgra = swapchainFormat == VK_FORMAT_B8G8R8A8_SRGB ||
                                  swapchainFormat == VK_FORMAT_B8G8R8A8_UNORM;
                rgba[index + 0] = bgra ? src[index + 2] : src[index + 0];
                rgba[index + 1] = src[index + 1];
                rgba[index + 2] = bgra ? src[index + 0] : src[index + 2];
                rgba[index + 3] = src[index + 3];
            }
        }
        std::error_code error;
        if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
        if (stbi_write_png(path.string().c_str(), static_cast<int>(swapchainExtent.width),
                static_cast<int>(swapchainExtent.height), 4, rgba.data(),
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
        stats.primitiveCount = demoResources.primitiveCount();
        if (config.enableClusteredLighting && packet.camera.viewportAndInvViewport.x > 0.0f &&
            packet.camera.viewportAndInvViewport.y > 0.0f)
        {
            Quality::ClusterGrid grid{};
            grid.tilesX = std::max(1u, static_cast<std::uint32_t>(
                std::ceil(packet.camera.viewportAndInvViewport.x / 64.0f)));
            grid.tilesY = std::max(1u, static_cast<std::uint32_t>(
                std::ceil(packet.camera.viewportAndInvViewport.y / 64.0f)));
            grid.slicesZ = 24;
            grid.nearPlane = std::max(1.0e-4f, packet.camera.positionAndNear.w);
            grid.farPlane = packet.camera.forwardAndFar.w > grid.nearPlane
                                ? packet.camera.forwardAndFar.w
                                : 1000.0f;
            std::vector<Quality::ClusterLight> lights;
            lights.reserve(packet.lights.size());
            for (std::size_t i = 0; i < packet.lights.size(); ++i)
            {
                const auto& light = packet.lights[i];
                lights.push_back(Quality::ClusterLight{
                    {light.positionAndRadius[0], light.positionAndRadius[1],
                        light.positionAndRadius[2]},
                    light.positionAndRadius[3],
                    static_cast<std::uint32_t>(i),
                    light.positionAndRadius[3] <= 0.0f});
            }
            const auto clustered = Quality::assignClusteredLights(grid,
                packet.camera.view,
                packet.camera.projection,
                {static_cast<std::uint32_t>(packet.camera.viewportAndInvViewport.x),
                    static_cast<std::uint32_t>(packet.camera.viewportAndInvViewport.y)},
                lights,
                128);
            stats.clusterOverflowCount = clustered.overflowCount;
        }
        stats.taaHistoryValid = config.enableTaa && taaHistoryValid && hasRenderedFrame &&
                                packet.frameIndex == lastFrameIndex + 1u;
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
                }
            }
        }
        frame.submitted = false;

        if (bindlessTable.initialized())
        {
            std::uint64_t completedTimeline = 0;
            if (vkGetSemaphoreCounterValue(
                    device, frameContext.timelineSemaphore, &completedTimeline) == VK_SUCCESS)
            {
                (void)bindlessTable.collect(completedTimeline);
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

        const VoidResult recordResult = recordFrame(frame, stats.swapchainImageIndex, packet);
        if (!recordResult)
        {
            setError(recordResult.error().describe());
            stats.deviceLost = deviceLost;
            initialized = false;
            fatalError = true;
            stats.fatalError = true;
            stats.cpuFrameMs = elapsedMilliseconds(begin);
            return stats;
        }

        result = frameContext.resetFence(device, frame);
        if (result != VK_SUCCESS)
        {
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
            setError(vkFailure("vkQueueSubmit2", result));
            deviceLost = deviceLost || result == VK_ERROR_DEVICE_LOST;
            initialized = false;
            fatalError = true;
            stats.deviceLost = deviceLost;
            stats.fatalError = true;
            stats.cpuFrameMs = elapsedMilliseconds(begin);
            return stats;
        }
        swapchainImageInitialized[stats.swapchainImageIndex] = true;
        depthImageInitialized = true;

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

        currentFrame = (currentFrame + 1) % static_cast<std::uint32_t>(frames.size());
        lastPresentedImage = stats.swapchainImageIndex;
        stats.rendered = true;
        // This field tracks renderer-owned allocations (currently the D32
        // depth target), not the physical heap capacity reported in
        // Capabilities.
        stats.deviceMemoryBytes = static_cast<std::uint64_t>(deviceMemoryBytes);
        stats.quality.rayQueryEnabled = rayQueryEnabled;
        // The current backend still records one Vulkan dynamic-rendering
        // scope.  Publish the canonical M3 pass names so capture/performance
        // tooling remains stable while the individual GPU implementations are
        // enabled incrementally.  Timestamp data is attached when available;
        // otherwise -1 explicitly means "not measured".
        if (stats.gpuPasses.size() == 1u && stats.gpuPasses.front().name == "Scene")
        {
            // The legacy timestamp bracket surrounds the opaque draw; expose
            // it under the corresponding canonical pass name.
            stats.gpuPasses.front().name = "G-buffer";
        }
        if (stats.gpuPasses.empty() || stats.gpuPasses.size() == 1u)
        {
            const std::array<const char*, 7> passNames = {
                "CSM shadows",
                "G-buffer",
                config.enableClusteredLighting ? "Clustered deferred lighting" : "Deferred lighting",
                config.enableTransparency ? "Forward transparency" : nullptr,
                config.enableTaa ? "TAA resolve" : "Copy HDR",
                "ACES tonemap",
                "Present"};
            for (const char* name : passNames)
            {
                if (name != nullptr &&
                    std::none_of(stats.gpuPasses.begin(), stats.gpuPasses.end(),
                        [name](const FrameStats::PassTiming& pass) { return pass.name == name; }))
                {
                    stats.gpuPasses.push_back({name, -1.0});
                }
            }
        }
        stats.taaHistoryValid = config.enableTaa && taaHistoryValid && hasRenderedFrame &&
                                packet.frameIndex == lastFrameIndex + 1u;
        if (config.enableTaa)
        {
            taaHistoryValid = true;
            hasRenderedFrame = true;
            lastFrameIndex = packet.frameIndex;
        }
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
    FrameContext& frame, std::uint32_t imageIndex, const FramePacket& packet)
{
    HALCYON_PROFILE_SCOPE("Renderer::recordFrame");
    (void)packet; // Scene uploads are introduced by the next milestone.
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
        vkCmdResetQueryPool(frame.commandBuffer,
            frameContext.timestampPool,
            frame.queryBase,
            2u + frameContext.maxPassCount * 2u);
        vkCmdWriteTimestamp2(frame.commandBuffer,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            frameContext.timestampPool,
            frame.queryBase);
    }

    if (depthImage == VK_NULL_HANDLE || depthImageView == VK_NULL_HANDLE)
    {
        return fail("Depth resources are not available for the active swapchain",
            Halcyon::ErrorCode::InvalidState);
    }
#if HALCYON_BUILD_EXPERIMENTAL_M2
    struct DynamicRenderingTarget
    {
        VkImageView colorView = VK_NULL_HANDLE;
        VkImageView depthView = VK_NULL_HANDLE;
    } dynamicTarget{swapchainImageViews[imageIndex], depthImageView};
#endif

#if HALCYON_BUILD_EXPERIMENTAL_M2
    auto recordScenePass = [&](const Graph::FrameGraphResources* frameGraphResources) -> VoidResult
#else
    auto recordScenePass = [&]() -> VoidResult
#endif
    {
        VkClearValue clearValue{};
        clearValue.color.float32[0] = 0.018f;
        clearValue.color.float32[1] = 0.028f;
        clearValue.color.float32[2] = 0.055f;
        clearValue.color.float32[3] = 1.0f;
        VkRenderingAttachmentInfo colorAttachment{};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachment.imageView = swapchainImageViews[imageIndex];
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue = clearValue;

        VkClearValue depthClear{};
        depthClear.depthStencil.depth = 0.0f;
        depthClear.depthStencil.stencil = 0;
        VkRenderingAttachmentInfo depthAttachment{};
        depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAttachment.imageView = depthImageView;
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depthAttachment.clearValue = depthClear;

        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea.offset = {0, 0};
        rendering.renderArea.extent = swapchainExtent;
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &colorAttachment;
        rendering.pDepthAttachment = &depthAttachment;
#if HALCYON_BUILD_EXPERIMENTAL_M2
        if (frameGraphResources != nullptr)
        {
            const auto passInfo = frameGraphResources->getRenderPassInfo(0);
            if (passInfo.target.token == &dynamicTarget)
            {
                colorAttachment.imageView = dynamicTarget.colorView;
                depthAttachment.imageView = dynamicTarget.depthView;
                colorAttachment.loadOp = any(passInfo.clearFlags & Graph::FrameGraphAttachmentFlags::Color0)
                                             ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                             : VK_ATTACHMENT_LOAD_OP_LOAD;
                depthAttachment.loadOp = any(passInfo.clearFlags & Graph::FrameGraphAttachmentFlags::Depth)
                                             ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                             : VK_ATTACHMENT_LOAD_OP_LOAD;
                colorAttachment.storeOp = any(passInfo.discardEnd & Graph::FrameGraphAttachmentFlags::Color0)
                                              ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                                              : VK_ATTACHMENT_STORE_OP_STORE;
                depthAttachment.storeOp = any(passInfo.discardEnd & Graph::FrameGraphAttachmentFlags::Depth)
                                              ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                                              : VK_ATTACHMENT_STORE_OP_STORE;
                colorAttachment.clearValue.color.float32[0] = passInfo.descriptor.clearColor.r;
                colorAttachment.clearValue.color.float32[1] = passInfo.descriptor.clearColor.g;
                colorAttachment.clearValue.color.float32[2] = passInfo.descriptor.clearColor.b;
                colorAttachment.clearValue.color.float32[3] = passInfo.descriptor.clearColor.a;
                rendering.renderArea.offset = {passInfo.descriptor.viewport.left,
                    passInfo.descriptor.viewport.top};
                rendering.renderArea.extent = {passInfo.descriptor.viewport.width,
                    passInfo.descriptor.viewport.height};
                rendering.layerCount = passInfo.descriptor.layerCount;
            }
        }
#endif
        vkCmdBeginRendering(frame.commandBuffer, &rendering);
        if (graphicsPipeline.pipeline() != VK_NULL_HANDLE)
        {
            VkViewport viewport{};
            viewport.x = 0.0f;
            viewport.y = 0.0f;
            viewport.width = static_cast<float>(swapchainExtent.width);
            viewport.height = static_cast<float>(swapchainExtent.height);
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            VkRect2D scissor{};
            scissor.offset = {0, 0};
            scissor.extent = swapchainExtent;
            vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
            vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
            vkCmdBindPipeline(
                frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline.pipeline());
            if (graphicsPipeline.textured() &&
                demoResources.demoMesh().vertexBuffer.buffer != VK_NULL_HANDLE)
            {
                // Rendering is driven entirely by the immutable packet.  The
                // backend does not own a scene camera or animation policy:
                // callers provide the camera matrix and (optionally) the first
                // instance transform.  Missing instance data means identity.
                const glm::mat4 viewProjection = packet.camera.viewProjection;
                glm::mat4 model{1.0f};
                // Static-scene uploads bake each node world transform into the
                // combined vertex stream.  Do not apply the first ECS
                // primitive transform a second time; legacy single-mesh
                // callers still use their packet model matrix.
                if (demoResources.sceneDraws().size() <= 1u && !packet.instances.empty())
                {
                    static_assert(sizeof(glm::mat4) == sizeof(std::array<float, 16>));
                    std::memcpy(glm::value_ptr(model),
                        packet.instances.front().transform.data(),
                        sizeof(model));
                }
                const TexturedPushConstants pushConstants{viewProjection, model};
                const VkDeviceSize offset = 0;
                const auto& demoMesh = demoResources.demoMesh();
                vkCmdBindVertexBuffers(
                    frame.commandBuffer, 0, 1, &demoMesh.vertexBuffer.buffer, &offset);
                vkCmdBindIndexBuffer(
                    frame.commandBuffer, demoMesh.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
                if (graphicsPipeline.bindless() && bindlessTable.descriptorSet() != VK_NULL_HANDLE)
                {
                    const VkDescriptorSet bindlessSet = bindlessTable.descriptorSet();
                    vkCmdBindDescriptorSets(frame.commandBuffer,
                        VK_PIPELINE_BIND_POINT_GRAPHICS,
                        graphicsPipeline.layout(),
                        1,
                        1,
                        &bindlessSet,
                        0,
                        nullptr);
                }
                vkCmdPushConstants(frame.commandBuffer,
                    graphicsPipeline.layout(),
                    VK_SHADER_STAGE_VERTEX_BIT,
                    0,
                    sizeof(TexturedPushConstants),
                    &pushConstants);
                const auto& draws = demoResources.sceneDraws();
                const auto& descriptors = demoResources.sceneTextureDescriptorSets();
                if (!draws.empty() && !descriptors.empty())
                {
                    for (const auto& draw : draws)
                    {
                        const std::uint32_t descriptorIndex =
                            std::min(draw.textureIndex,
                                static_cast<std::uint32_t>(descriptors.size() - 1u));
                        const VkDescriptorSet textureDescriptorSet = descriptors[descriptorIndex];
                        vkCmdBindDescriptorSets(frame.commandBuffer,
                            VK_PIPELINE_BIND_POINT_GRAPHICS,
                            graphicsPipeline.layout(),
                            0,
                            1,
                            &textureDescriptorSet,
                            0,
                            nullptr);
                        vkCmdDrawIndexed(frame.commandBuffer,
                            draw.indexCount,
                            1,
                            draw.firstIndex,
                            draw.vertexOffset,
                            0);
                    }
                }
                else
                {
                    const VkDescriptorSet textureDescriptorSet = demoResources.textureDescriptorSet();
                    if (textureDescriptorSet != VK_NULL_HANDLE)
                    {
                        vkCmdBindDescriptorSets(frame.commandBuffer,
                            VK_PIPELINE_BIND_POINT_GRAPHICS,
                            graphicsPipeline.layout(),
                            0,
                            1,
                            &textureDescriptorSet,
                            0,
                            nullptr);
                    }
                    vkCmdDrawIndexed(frame.commandBuffer, demoMesh.indexCount, 1, 0, 0, 0);
                }
            }
            else if (demoResources.triangleVertexBuffer().buffer != VK_NULL_HANDLE)
            {
                const VkDeviceSize offset = 0;
                const VkBuffer triangleBuffer = demoResources.triangleVertexBuffer().buffer;
                vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &triangleBuffer, &offset);
                vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
            }
        }
        if (overlayCallback != nullptr)
        {
            overlayCallback(frame.commandBuffer);
        }
        vkCmdEndRendering(frame.commandBuffer);
        return ok();
    };

#if HALCYON_BUILD_EXPERIMENTAL_M2
    Graph::FrameGraph graph;
    Graph::BarrierPlanner barrierPlanner;
    bool sceneRecordFailed = false;
    std::string sceneRecordError;
    Graph::FrameGraphRenderPass::ImportDescriptor importedTargetDescriptor{};
    importedTargetDescriptor.attachments = Graph::FrameGraphAttachmentFlags::Color0 |
                                           Graph::FrameGraphAttachmentFlags::Depth;
    importedTargetDescriptor.viewport.width = swapchainExtent.width;
    importedTargetDescriptor.viewport.height = swapchainExtent.height;
    importedTargetDescriptor.clearColor = {0.018f, 0.028f, 0.055f, 1.0f};
    importedTargetDescriptor.clearFlags = Graph::FrameGraphAttachmentFlags::Color0 |
                                          Graph::FrameGraphAttachmentFlags::Depth;
    importedTargetDescriptor.keepOverrideStart = Graph::FrameGraphAttachmentFlags::All;
    importedTargetDescriptor.keepOverrideEnd = Graph::FrameGraphAttachmentFlags::All;
    const auto color = graph.import("SwapchainRenderTarget", importedTargetDescriptor,
        Graph::FrameGraphNativeResource{&dynamicTarget});
    const auto depth = graph.importTexture(Graph::TextureDesc{.name = "Depth",
        .width = swapchainExtent.width,
        .height = swapchainExtent.height,
        .depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = Graph::TextureFormat::D32Float,
        .transient = false});
    graph.addPass<Graph::FrameGraph::Empty>("Scene",
        [&](Graph::FrameGraph::Builder& builder, Graph::FrameGraph::Empty&)
        {
            const auto colorOutput = builder.write(color, Graph::ResourceUsage::ColorAttachment);
            const auto depthOutput = builder.write(depth, Graph::ResourceUsage::DepthAttachment);
            Graph::FrameGraphRenderPass::Descriptor descriptor{};
            descriptor.attachments.color[0] = colorOutput;
            descriptor.attachments.depth = depthOutput;
            descriptor.viewport.width = swapchainExtent.width;
            descriptor.viewport.height = swapchainExtent.height;
            descriptor.clearColor = importedTargetDescriptor.clearColor;
            descriptor.clearFlags = importedTargetDescriptor.clearFlags;
            builder.declareRenderPass("Scene", descriptor);
            builder.sideEffect();
        },
        [&](const Graph::FrameGraphResources& resources,
            const Graph::FrameGraph::Empty&,
            Graph::CommandContext&)
        {
            HALCYON_PROFILE_SCOPE("FrameGraph::pass");
            const VoidResult result = recordScenePass(&resources);
            if (!result)
            {
                sceneRecordFailed = true;
                sceneRecordError = result.error().describe();
            }
        });

    const Graph::CompileResult compiled = graph.compile();
    if (!compiled)
    {
        return fail(compiled.error.message, Halcyon::ErrorCode::InvalidState);
    }
    frame.passNames.clear();
    frame.passNames.reserve(compiled.executionOrder.size());
    for (const auto handle : compiled.executionOrder)
    {
        if (const auto* pass = compiled.pass(handle); pass != nullptr)
        {
            frame.passNames.push_back(pass->name);
        }
    }
    barrierPlanner.begin();
    barrierPlanner.setState(Graph::ResourceKind::Texture, color.index(), color.version(),
        Graph::BarrierState{Graph::PipelineStage::None,
            Graph::AccessFlags::None,
            swapchainImageInitialized[imageIndex] ? Graph::ImageLayout::Present
                                                   : Graph::ImageLayout::Undefined,
            Graph::QueueClass::Graphics,
            false});
    barrierPlanner.setState(Graph::ResourceKind::Texture, depth.index(), depth.version(),
        Graph::BarrierState{depthImageInitialized ? Graph::PipelineStage::DepthTest
                                                  : Graph::PipelineStage::None,
            depthImageInitialized ? (Graph::AccessFlags::DepthRead | Graph::AccessFlags::DepthWrite)
                                  : Graph::AccessFlags::None,
            depthImageInitialized ? Graph::ImageLayout::DepthAttachment
                                  : Graph::ImageLayout::Undefined,
            Graph::QueueClass::Graphics,
            depthImageInitialized});
    Graph::ExecuteOptions executeOptions;
    executeOptions.userData = &frame;
    executeOptions.onBegin = [this, &frame, &compiled, &barrierPlanner, color, depth, imageIndex](
                                 const Graph::PassExecutionContext& context)
    {
        if (const auto* pass = compiled.pass(context.handle); pass != nullptr)
        {
            // The planner translates semantic graph accesses into a compact
            // Barrier2 intent before the Vulkan-specific frame barriers are
            // emitted.  Keeping this step at the pass boundary ensures the
            // graph and command recording cannot silently diverge.
            const auto barriers = barrierPlanner.plan(pass->accesses, pass->queue);
            std::vector<VkImageMemoryBarrier2> imageBarriers;
            imageBarriers.reserve(barriers.size());
            for (const auto& barrier : barriers)
            {
                if (!barrier.required || barrier.access.kind != Graph::ResourceKind::Texture)
                {
                    continue;
                }
                VkImage image = VK_NULL_HANDLE;
                VkImageAspectFlags aspect = 0;
                if (barrier.access.resourceIndex == color.index())
                {
                    image = swapchainImages[imageIndex];
                    aspect = VK_IMAGE_ASPECT_COLOR_BIT;
                }
                else if (barrier.access.resourceIndex == depth.index())
                {
                    image = depthImage;
                    aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
                }
                if (image == VK_NULL_HANDLE)
                {
                    continue;
                }
                VkImageMemoryBarrier2 vkBarrier{};
                vkBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                vkBarrier.srcStageMask = toVkStages(barrier.before.stage);
                vkBarrier.srcAccessMask = toVkAccess(barrier.before.access);
                vkBarrier.dstStageMask = toVkStages(barrier.after.stage);
                vkBarrier.dstAccessMask = toVkAccess(barrier.after.access);
                vkBarrier.oldLayout = toVkLayout(barrier.before.layout);
                vkBarrier.newLayout = toVkLayout(barrier.after.layout);
                vkBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                vkBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                vkBarrier.image = image;
                vkBarrier.subresourceRange.aspectMask = aspect;
                vkBarrier.subresourceRange.levelCount = 1;
                vkBarrier.subresourceRange.layerCount = 1;
                imageBarriers.push_back(vkBarrier);
            }
            if (!imageBarriers.empty())
            {
                VkDependencyInfo dependency{};
                dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                dependency.imageMemoryBarrierCount =
                    static_cast<std::uint32_t>(imageBarriers.size());
                dependency.pImageMemoryBarriers = imageBarriers.data();
                vkCmdPipelineBarrier2(frame.commandBuffer, &dependency);
            }
        }
        (void)frameContext.writePassTimestamp(
            frame.commandBuffer, frame, context.executionIndex, true);
    };
    executeOptions.onEnd = [this, &frame](const Graph::PassExecutionContext& context)
    {
        (void)frameContext.writePassTimestamp(
            frame.commandBuffer, frame, context.executionIndex, false);
    };
    Graph::CommandContext graphCommands;
    graph.execute(graphCommands, executeOptions);
    if (graph.lastError())
    {
        return fail(graph.lastError().message, Halcyon::ErrorCode::InvalidState);
    }
    if (sceneRecordFailed)
    {
        return fail(sceneRecordError, Halcyon::ErrorCode::InvalidState);
    }
#else
    const VoidResult sceneResult = recordScenePass();
    if (!sceneResult)
    {
        return sceneResult;
    }
#endif

    // Keep the depth image in DEPTH_ATTACHMENT_OPTIMAL between frames.  The
    // frame fence serialises reuse, so no needless layout churn is required;
    // the first-use barrier above is emitted again after every resize.

    VkImageMemoryBarrier2 presentBarrier{};
    presentBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    presentBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    presentBarrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    presentBarrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    presentBarrier.dstAccessMask = VK_ACCESS_2_NONE;
    presentBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    presentBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    presentBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    presentBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    presentBarrier.image = swapchainImages[imageIndex];
    presentBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    presentBarrier.subresourceRange.baseMipLevel = 0;
    presentBarrier.subresourceRange.levelCount = 1;
    presentBarrier.subresourceRange.baseArrayLayer = 0;
    presentBarrier.subresourceRange.layerCount = 1;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &presentBarrier;
    vkCmdPipelineBarrier2(frame.commandBuffer, &dependency);

    if (timestampsEnabled)
    {
        vkCmdWriteTimestamp2(frame.commandBuffer,
            VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
            frameContext.timestampPool,
            frame.queryBase + 1);
    }
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
        result =
            impl_->gpuAllocator.initialize(impl_->instance, impl_->physicalDevice, impl_->device);
        if (!result)
        {
            impl_->setError(result.error().describe());
            impl_->cleanup();
            return result;
        }
#if HALCYON_BUILD_EXPERIMENTAL_M2
        if (impl_->caps.descriptorIndexing)
        {
            const auto bindlessResult = impl_->bindlessTable.initialize(
                impl_->device, bindlessConfig(impl_->physicalProperties.limits));
            impl_->caps.bindlessTable = static_cast<bool>(bindlessResult);
        }
#endif
        result = impl_->swapchainState.initialize(impl_->physicalDevice,
            impl_->device,
            impl_->surface,
            window,
            impl_->graphicsQueueFamily,
            impl_->presentQueueFamily,
            impl_->gpuAllocator,
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
        result = impl_->demoResources.initialize(impl_->device,
            impl_->frames.front().commandPool,
            impl_->graphicsQueue,
            impl_->gpuAllocator,
            impl_->gpuUploader,
            impl_->config.startupTexturePath,
            impl_->config.startupMeshPath,
            impl_->config.startupScenePath);
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

Halcyon::Result<TextureResource> Renderer::loadTexture2D(const char* path)
{
    if (impl_ == nullptr || path == nullptr)
    {
        return Halcyon::Result<TextureResource>::failure(
            {Halcyon::ErrorCode::InvalidArgument, "Texture path is null"});
    }
    return impl_->demoResources.loadTexture2D(path);
}

Halcyon::Result<MeshResource> Renderer::loadObj(const char* path)
{
    if (impl_ == nullptr || path == nullptr)
    {
        return Halcyon::Result<MeshResource>::failure(
            {Halcyon::ErrorCode::InvalidArgument, "Model path is null"});
    }
    return impl_->demoResources.loadObj(path);
}

Halcyon::Result<void> Renderer::loadStaticScene(const char* path)
{
    if (impl_ == nullptr || path == nullptr)
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::InvalidArgument, "Scene path is null"});
    }
    impl_->demoResources.cleanup();
    auto result = impl_->demoResources.initialize(impl_->device,
        impl_->frames.front().commandPool,
        impl_->graphicsQueue,
        impl_->gpuAllocator,
        impl_->gpuUploader,
        nullptr,
        nullptr,
        path);
    if (result)
    {
        impl_->graphicsPipeline.destroy();
        result = impl_->createGraphicsPipeline();
    }
    return result;
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

void Renderer::destroy(TextureResource& texture) noexcept
{
    if (impl_ != nullptr)
    {
        impl_->demoResources.destroy(texture);
    }
}

void Renderer::destroy(MeshResource& mesh) noexcept
{
    if (impl_ != nullptr)
    {
        impl_->demoResources.destroy(mesh);
    }
}

VkInstance Renderer::instance() const noexcept
{
    return impl_ != nullptr ? impl_->instance : VK_NULL_HANDLE;
}

VkPhysicalDevice Renderer::physicalDevice() const noexcept
{
    return impl_ != nullptr ? impl_->physicalDevice : VK_NULL_HANDLE;
}

VkDevice Renderer::device() const noexcept
{
    return impl_ != nullptr ? impl_->device : VK_NULL_HANDLE;
}

VkQueue Renderer::graphicsQueue() const noexcept
{
    return impl_ != nullptr ? impl_->graphicsQueue : VK_NULL_HANDLE;
}

VkQueue Renderer::presentQueue() const noexcept
{
    return impl_ != nullptr ? impl_->presentQueue : VK_NULL_HANDLE;
}

VkFormat Renderer::swapchainFormat() const noexcept
{
    return impl_ != nullptr ? impl_->swapchainFormat : VK_FORMAT_UNDEFINED;
}

VkFormat Renderer::depthFormat() const noexcept
{
    return impl_ != nullptr ? impl_->depthFormat : VK_FORMAT_UNDEFINED;
}

VkExtent2D Renderer::swapchainExtent() const noexcept
{
    return impl_ != nullptr ? impl_->swapchainExtent : VkExtent2D{};
}

std::uint32_t Renderer::swapchainImageCount() const noexcept
{
    return impl_ != nullptr ? static_cast<std::uint32_t>(impl_->swapchainImages.size()) : 0u;
}

void Renderer::setOverlayCallback(OverlayCallback callback) noexcept
{
    if (impl_ != nullptr)
    {
        impl_->overlayCallback = callback;
    }
}

} // namespace Halcyon::Vulkan
