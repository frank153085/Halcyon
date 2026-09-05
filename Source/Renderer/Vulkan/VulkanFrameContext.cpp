#include "VulkanFrameContext.h"

#include "VulkanCommon.h"

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

namespace Halcyon::Vulkan
{
namespace
{
constexpr std::uint32_t kDefaultFramesInFlight = 3;
constexpr std::uint32_t kMaxFramesInFlight = 4;
} // namespace

Halcyon::Result<void> VulkanFrameContext::initialize(VkDevice device,
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceProperties& physicalProperties,
    std::uint32_t graphicsQueueFamily,
    std::uint32_t requestedFrameCount,
    std::uint32_t requestedPassCount)
{
    cleanup(device);
    maxPassCount = std::clamp(requestedPassCount, 1u, 64u);
    auto result = createTimeline(device);
    if (!result)
    {
        return result;
    }
    result = createResources(device,
        physicalDevice,
        physicalProperties,
        graphicsQueueFamily,
        requestedFrameCount,
        maxPassCount);
    if (!result)
    {
        cleanup(device);
    }
    return result;
}

Halcyon::Result<void> VulkanFrameContext::createTimeline(VkDevice device)
{
    VkSemaphoreTypeCreateInfo typeInfo{};
    typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    typeInfo.initialValue = 0;
    VkSemaphoreCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    createInfo.pNext = &typeInfo;
    const VkResult result = vkCreateSemaphore(device, &createInfo, nullptr, &timelineSemaphore);
    if (result != VK_SUCCESS)
    {
        return fail(vkFailure("vkCreateSemaphore(timeline)", result));
    }
    return ok();
}

Halcyon::Result<void> VulkanFrameContext::createResources(VkDevice device,
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceProperties& physicalProperties,
    std::uint32_t graphicsQueueFamily,
    std::uint32_t requestedFrameCount,
    std::uint32_t requestedPassCount)
{
    const std::uint32_t passCount = std::clamp(requestedPassCount, 1u, 64u);
    maxPassCount = passCount;
    const std::uint32_t frameCount =
        std::clamp(requestedFrameCount == 0 ? kDefaultFramesInFlight : requestedFrameCount,
            2u,
            kMaxFramesInFlight);
    frames.resize(frameCount);
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = graphicsQueueFamily;
    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (std::uint32_t i = 0; i < frameCount; ++i)
    {
        auto& frame = frames[i];
        VkResult result = vkCreateCommandPool(device, &poolInfo, nullptr, &frame.commandPool);
        if (result != VK_SUCCESS)
        {
            return fail(vkFailure("vkCreateCommandPool", result));
        }
        allocateInfo.commandPool = frame.commandPool;
        result = vkAllocateCommandBuffers(device, &allocateInfo, &frame.commandBuffer);
        if (result != VK_SUCCESS)
        {
            return fail(vkFailure("vkAllocateCommandBuffers", result));
        }
        result = vkCreateSemaphore(device, &semaphoreInfo, nullptr, &frame.imageAvailable);
        if (result != VK_SUCCESS)
        {
            return fail(vkFailure("vkCreateSemaphore(imageAvailable)", result));
        }
        result = vkCreateFence(device, &fenceInfo, nullptr, &frame.fence);
        if (result != VK_SUCCESS)
        {
            return fail(vkFailure("vkCreateFence", result));
        }
        frame.queryBase = i * (2u + passCount * 2u + VulkanFrameContext::StageQueryCount);
        frame.passQueryBase = frame.queryBase + 2u;
        frame.stageQueryBase = frame.passQueryBase + passCount * 2u;
    }
    std::uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(
        physicalDevice, &queueFamilyCount, queueFamilies.data());
    if (graphicsQueueFamily < queueFamilies.size())
    {
        presentTimestampValidBits = queueFamilies[graphicsQueueFamily].timestampValidBits;
    }
    timestampPeriod = physicalProperties.limits.timestampPeriod;
    if (presentTimestampValidBits != 0 && timestampPeriod > 0.0f)
    {
        VkQueryPoolCreateInfo queryInfo{};
        queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queryInfo.queryCount = frameCount * (2u + passCount * 2u + VulkanFrameContext::StageQueryCount);
        const VkResult result = vkCreateQueryPool(device, &queryInfo, nullptr, &timestampPool);
        if (result == VK_SUCCESS)
        {
            timestampsEnabled = true;
        }
    }
    return ok();
}

void VulkanFrameContext::cleanup(VkDevice device) noexcept
{
    if (device != VK_NULL_HANDLE)
    {
        if (timestampPool != VK_NULL_HANDLE)
        {
            vkDestroyQueryPool(device, timestampPool, nullptr);
        }
        for (auto& frame : frames)
        {
            if (frame.fence != VK_NULL_HANDLE)
            {
                vkDestroyFence(device, frame.fence, nullptr);
            }
            if (frame.imageAvailable != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(device, frame.imageAvailable, nullptr);
            }
            if (frame.commandPool != VK_NULL_HANDLE)
            {
                vkDestroyCommandPool(device, frame.commandPool, nullptr);
            }
        }
        if (timelineSemaphore != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(device, timelineSemaphore, nullptr);
        }
    }
    timestampPool = VK_NULL_HANDLE;
    timelineSemaphore = VK_NULL_HANDLE;
    frames.clear();
    currentFrame = 0;
    nextTimelineValue = 1;
    timestampsEnabled = false;
    timestampPeriod = 1.0f;
    presentTimestampValidBits = 0;
    maxPassCount = 16;
}

VkResult VulkanFrameContext::submit(
    VkQueue graphicsQueue, VulkanFrame& frame, VkSemaphore presentReady) noexcept
{
    if (graphicsQueue == VK_NULL_HANDLE || frame.commandBuffer == VK_NULL_HANDLE ||
        frame.imageAvailable == VK_NULL_HANDLE || frame.fence == VK_NULL_HANDLE ||
        presentReady == VK_NULL_HANDLE || timelineSemaphore == VK_NULL_HANDLE)
    {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const std::uint64_t signalValue = nextTimelineValue;
    VkCommandBufferSubmitInfo commandInfo{};
    commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    commandInfo.commandBuffer = frame.commandBuffer;

    VkSemaphoreSubmitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waitInfo.semaphore = frame.imageAvailable;
    waitInfo.value = 0; // binary semaphore
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    std::array<VkSemaphoreSubmitInfo, 2> signalInfos{};
    signalInfos[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfos[0].semaphore = presentReady;
    signalInfos[0].value = 0; // binary semaphore
    signalInfos[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signalInfos[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfos[1].semaphore = timelineSemaphore;
    signalInfos[1].value = signalValue;
    signalInfos[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submitInfo.waitSemaphoreInfoCount = 1;
    submitInfo.pWaitSemaphoreInfos = &waitInfo;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &commandInfo;
    submitInfo.signalSemaphoreInfoCount = static_cast<std::uint32_t>(signalInfos.size());
    submitInfo.pSignalSemaphoreInfos = signalInfos.data();

    const VkResult result = vkQueueSubmit2(graphicsQueue, 1, &submitInfo, frame.fence);
    if (result == VK_SUCCESS)
    {
        frame.timelineValue = signalValue;
        frame.submitted = true;
        ++nextTimelineValue;
    }
    return result;
}

VkResult VulkanFrameContext::wait(VkDevice device, VulkanFrame& frame) const noexcept
{
    if (device == VK_NULL_HANDLE || frame.fence == VK_NULL_HANDLE)
    {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return vkWaitForFences(
        device, 1, &frame.fence, VK_TRUE, std::numeric_limits<std::uint64_t>::max());
}

VkResult VulkanFrameContext::resetCommandPool(VkDevice device, VulkanFrame& frame) const noexcept
{
    if (device == VK_NULL_HANDLE || frame.commandPool == VK_NULL_HANDLE)
    {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return vkResetCommandPool(device, frame.commandPool, 0);
}

VkResult VulkanFrameContext::resetFence(VkDevice device, VulkanFrame& frame) const noexcept
{
    if (device == VK_NULL_HANDLE || frame.fence == VK_NULL_HANDLE)
    {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return vkResetFences(device, 1, &frame.fence);
}

VkResult VulkanFrameContext::readGpuTime(
    VkDevice device, const VulkanFrame& frame, double& milliseconds) const noexcept
{
    if (!timestampsEnabled)
    {
        return VK_NOT_READY;
    }
    if (device == VK_NULL_HANDLE || timestampPool == VK_NULL_HANDLE)
    {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    std::array<std::uint64_t, 2> timestampValues{};
    const VkResult result = vkGetQueryPoolResults(device,
        timestampPool,
        frame.queryBase,
        2,
        sizeof(timestampValues),
        timestampValues.data(),
        sizeof(std::uint64_t),
        VK_QUERY_RESULT_64_BIT);
    if (result != VK_SUCCESS)
    {
        return result;
    }
    const std::uint64_t validMask = presentTimestampValidBits >= 64
                                        ? ~std::uint64_t{0}
                                        : ((std::uint64_t{1} << presentTimestampValidBits) - 1u);
    const std::uint64_t startTimestamp = timestampValues[0] & validMask;
    const std::uint64_t endTimestamp = timestampValues[1] & validMask;
    const std::uint64_t elapsedTicks = (endTimestamp - startTimestamp) & validMask;
    milliseconds =
        static_cast<double>(elapsedTicks) * static_cast<double>(timestampPeriod) / 1'000'000.0;
    return VK_SUCCESS;
}

VkResult VulkanFrameContext::readPassTime(VkDevice device,
    const VulkanFrame& frame,
    std::uint32_t passIndex,
    double& milliseconds) const noexcept
{
    if (!timestampsEnabled)
    {
        return VK_NOT_READY;
    }
    if (device == VK_NULL_HANDLE || timestampPool == VK_NULL_HANDLE || passIndex >= maxPassCount)
    {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    std::array<std::uint64_t, 2> timestampValues{};
    const VkResult result = vkGetQueryPoolResults(device,
        timestampPool,
        frame.passQueryBase + passIndex * 2u,
        2,
        sizeof(timestampValues),
        timestampValues.data(),
        sizeof(std::uint64_t),
        VK_QUERY_RESULT_64_BIT);
    if (result != VK_SUCCESS)
    {
        return result;
    }
    const std::uint64_t validMask = presentTimestampValidBits >= 64
                                        ? ~std::uint64_t{0}
                                        : ((std::uint64_t{1} << presentTimestampValidBits) - 1u);
    const std::uint64_t elapsedTicks =
        ((timestampValues[1] & validMask) - (timestampValues[0] & validMask)) & validMask;
    milliseconds =
        static_cast<double>(elapsedTicks) * static_cast<double>(timestampPeriod) / 1'000'000.0;
    return VK_SUCCESS;
}

VkResult VulkanFrameContext::readStageTime(VkDevice device, const VulkanFrame& frame,
    std::uint32_t stageIndex, double& milliseconds) const noexcept
{
    if (!timestampsEnabled) return VK_NOT_READY;
    if (device == VK_NULL_HANDLE || timestampPool == VK_NULL_HANDLE || stageIndex >= 4u)
        return VK_ERROR_INITIALIZATION_FAILED;
    std::array<std::uint64_t, 2> values{};
    const VkResult result = vkGetQueryPoolResults(device, timestampPool,
        frame.stageQueryBase + stageIndex * 2u, 2, sizeof(values), values.data(),
        sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
    if (result != VK_SUCCESS) return result;
    const std::uint64_t validMask = presentTimestampValidBits >= 64
        ? ~std::uint64_t{0}
        : ((std::uint64_t{1} << presentTimestampValidBits) - 1u);
    const std::uint64_t elapsed = ((values[1] & validMask) - (values[0] & validMask)) & validMask;
    milliseconds = static_cast<double>(elapsed) * static_cast<double>(timestampPeriod) / 1'000'000.0;
    return VK_SUCCESS;
}

bool VulkanFrameContext::writePassTimestamp(VkCommandBuffer commandBuffer,
    const VulkanFrame& frame,
    std::uint32_t passIndex,
    bool begin) const noexcept
{
    if (!timestampsEnabled || commandBuffer == VK_NULL_HANDLE || passIndex >= maxPassCount)
    {
        return false;
    }
    vkCmdWriteTimestamp2(commandBuffer,
        begin ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
        timestampPool,
        frame.passQueryBase + passIndex * 2u + (begin ? 0u : 1u));
    return true;
}

VkResult VulkanFrameContext::present(VkQueue presentQueue,
    VkSwapchainKHR swapchain,
    VkSemaphore presentReady,
    std::uint32_t imageIndex) noexcept
{
    if (presentQueue == VK_NULL_HANDLE || swapchain == VK_NULL_HANDLE ||
        presentReady == VK_NULL_HANDLE)
    {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &presentReady;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain;
    presentInfo.pImageIndices = &imageIndex;
    return vkQueuePresentKHR(presentQueue, &presentInfo);
}

} // namespace Halcyon::Vulkan
