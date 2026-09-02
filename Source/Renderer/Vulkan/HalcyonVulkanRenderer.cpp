#include "HalcyonVulkanRenderer.h"

#include "GpuAllocator.h"
#include "GpuUploader.h"
#include "VulkanCommon.h"
#include "VulkanDemoResources.h"
#include "VulkanDevice.h"
#include "VulkanFrameContext.h"
#include "VulkanPipeline.h"
#include "VulkanSwapchain.h"

// GLFW is included here (rather than in the public header) so applications
// can choose their own GLFW include policy.  The Vulkan include guard makes
// this safe when the caller included glfw3.h with GLFW_INCLUDE_VULKAN first.
#ifndef GLFW_INCLUDE_VULKAN
#define GLFW_INCLUDE_VULKAN
#endif
#include <GLFW/glfw3.h>
#include <array>
#include <chrono>
#include <cstring>
#include <exception>
#include <glm/gtc/type_ptr.hpp>
#include <new>
#include <utility>
#include <vector>

namespace Halcyon::Vulkan
{

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
        return pipelineResult;
    }

    [[nodiscard]] VoidResult createGraphicsPipeline()
    {
        const auto result = graphicsPipeline.create(device,
            swapchainFormat,
            depthFormat,
            swapchainExtent,
            demoResources.textureSetLayout(),
            demoResources.textured());
        // A textured pipeline can be unavailable (for example when its
        // generated SPIR-V is missing); VulkanPipeline falls back to the
        // embedded triangle shaders and reports the active mode.
        return result;
    }

    [[nodiscard]] VoidResult recordFrame(
        FrameContext& frame, std::uint32_t imageIndex, const FramePacket& packet);

    [[nodiscard]] FrameStats render(const FramePacket& packet)
    {
        FrameStats stats{};
        stats.quality.rayQueryEnabled = rayQueryEnabled;
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
        }
        frame.submitted = false;

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
        stats.rendered = true;
        // This field tracks renderer-owned allocations (currently the D32
        // depth target), not the physical heap capacity reported in
        // Capabilities.
        stats.deviceMemoryBytes = static_cast<std::uint64_t>(deviceMemoryBytes);
        stats.quality.rayQueryEnabled = rayQueryEnabled;
        stats.cpuFrameMs = elapsedMilliseconds(begin);
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
        vkCmdResetQueryPool(frame.commandBuffer, frameContext.timestampPool, frame.queryBase, 2);
        vkCmdWriteTimestamp2(frame.commandBuffer,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            frameContext.timestampPool,
            frame.queryBase);
    }

    const bool initializedImage = swapchainImageInitialized[imageIndex];
    VkImageMemoryBarrier2 acquireBarrier{};
    acquireBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    acquireBarrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    acquireBarrier.srcAccessMask = VK_ACCESS_2_NONE;
    acquireBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    acquireBarrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    acquireBarrier.oldLayout =
        initializedImage ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED;
    acquireBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    acquireBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    acquireBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    acquireBarrier.image = swapchainImages[imageIndex];
    acquireBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    acquireBarrier.subresourceRange.baseMipLevel = 0;
    acquireBarrier.subresourceRange.levelCount = 1;
    acquireBarrier.subresourceRange.baseArrayLayer = 0;
    acquireBarrier.subresourceRange.layerCount = 1;

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &acquireBarrier;
    vkCmdPipelineBarrier2(frame.commandBuffer, &dependency);

    if (depthImage == VK_NULL_HANDLE || depthImageView == VK_NULL_HANDLE)
    {
        return fail("Depth resources are not available for the active swapchain",
            Halcyon::ErrorCode::InvalidState);
    }
    // The depth target is shared by the frames-in-flight.  Queue submission
    // order prevents simultaneous execution, but a submission boundary is
    // not itself a memory dependency.  Emit a same-layout barrier on every
    // frame so a previous attachment write is visible before the next clear
    // and depth test; the first use still performs the UNDEFINED transition.
    VkImageMemoryBarrier2 depthBarrier{};
    depthBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    depthBarrier.srcStageMask = depthImageInitialized
                                    ? (VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                          VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT)
                                    : VK_PIPELINE_STAGE_2_NONE;
    depthBarrier.srcAccessMask = depthImageInitialized
                                     ? (VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                           VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)
                                     : VK_ACCESS_2_NONE;
    depthBarrier.dstStageMask =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    depthBarrier.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depthBarrier.oldLayout = depthImageInitialized ? VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL
                                                   : VK_IMAGE_LAYOUT_UNDEFINED;
    depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depthBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depthBarrier.image = depthImage;
    depthBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthBarrier.subresourceRange.baseMipLevel = 0;
    depthBarrier.subresourceRange.levelCount = 1;
    depthBarrier.subresourceRange.baseArrayLayer = 0;
    depthBarrier.subresourceRange.layerCount = 1;
    dependency.pImageMemoryBarriers = &depthBarrier;
    vkCmdPipelineBarrier2(frame.commandBuffer, &dependency);

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
            if (!packet.instances.empty())
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
            const VkDescriptorSet textureDescriptorSet = demoResources.textureDescriptorSet();
            vkCmdBindDescriptorSets(frame.commandBuffer,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                graphicsPipeline.layout(),
                0,
                1,
                &textureDescriptorSet,
                0,
                nullptr);
            vkCmdPushConstants(frame.commandBuffer,
                graphicsPipeline.layout(),
                VK_SHADER_STAGE_VERTEX_BIT,
                0,
                sizeof(TexturedPushConstants),
                &pushConstants);
            vkCmdDrawIndexed(frame.commandBuffer, demoMesh.indexCount, 1, 0, 0, 0);
        }
        else if (demoResources.triangleVertexBuffer().buffer != VK_NULL_HANDLE)
        {
            const VkDeviceSize offset = 0;
            const VkBuffer triangleBuffer = demoResources.triangleVertexBuffer().buffer;
            vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &triangleBuffer, &offset);
            vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
        }
    }
    vkCmdEndRendering(frame.commandBuffer);

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
    presentBarrier.subresourceRange = acquireBarrier.subresourceRange;
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
            impl_->config.startupMeshPath);
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

} // namespace Halcyon::Vulkan
