#include "GpuUploader.h"

#include <cstring>

namespace Halcyon::Vulkan
{
namespace
{

[[nodiscard]] Halcyon::Error makeVkError(const char* operation, VkResult result)
{
    const Halcyon::ErrorCode code =
        result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY
            ? Halcyon::ErrorCode::OutOfMemory
        : result == VK_ERROR_DEVICE_LOST ? Halcyon::ErrorCode::DeviceLost
                                         : Halcyon::ErrorCode::Backend;
    return {code, operation};
}

[[nodiscard]] Halcyon::Result<VkCommandBuffer> beginOneShot(
    VkDevice device, VkCommandPool commandPool)
{
    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = commandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkResult result = vkAllocateCommandBuffers(device, &allocateInfo, &commandBuffer);
    if (result != VK_SUCCESS)
    {
        return Halcyon::Result<VkCommandBuffer>::failure(
            makeVkError("vkAllocateCommandBuffers(upload)", result));
    }
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(commandBuffer, &beginInfo);
    if (result != VK_SUCCESS)
    {
        vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
        return Halcyon::Result<VkCommandBuffer>::failure(
            makeVkError("vkBeginCommandBuffer(upload)", result));
    }
    return commandBuffer;
}

[[nodiscard]] Halcyon::Result<void> submitOneShot(
    VkDevice device, VkCommandPool commandPool, VkQueue queue, VkCommandBuffer commandBuffer)
{
    VkResult result = vkEndCommandBuffer(commandBuffer);
    if (result == VK_SUCCESS)
    {
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        result = vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    }
    if (result == VK_SUCCESS)
    {
        result = vkQueueWaitIdle(queue);
    }
    vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
    return result == VK_SUCCESS
               ? Halcyon::Result<void>::success()
               : Halcyon::Result<void>::failure(makeVkError("upload command submission", result));
}

} // namespace

Halcyon::Result<void> GpuUploader::uploadBuffer(VkDevice device,
    VkCommandPool commandPool,
    VkQueue queue,
    GpuAllocator& allocator,
    BufferAllocation destination,
    std::span<const std::byte> data)
{
    if (device == VK_NULL_HANDLE || commandPool == VK_NULL_HANDLE || queue == VK_NULL_HANDLE ||
        data.empty() || destination.buffer == VK_NULL_HANDLE)
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::InvalidArgument, "Invalid buffer upload parameters"});
    }
    VkBufferCreateInfo stagingInfo{};
    stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagingInfo.size = data.size_bytes();
    stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    const auto stagingResult = allocator.createBuffer(stagingInfo, MemoryUsage::CpuToGpu);
    if (!stagingResult)
    {
        return stagingResult.error();
    }
    const BufferAllocation staging = stagingResult.value();
    const auto writeResult = allocator.writeBuffer(staging, data);
    if (!writeResult)
    {
        allocator.destroy(staging);
        return writeResult;
    }
    const auto commandResult = beginOneShot(device, commandPool);
    if (!commandResult)
    {
        allocator.destroy(staging);
        return commandResult.error();
    }
    const VkCommandBuffer commandBuffer = commandResult.value();
    VkBufferCopy copy{};
    copy.size = data.size_bytes();
    vkCmdCopyBuffer(commandBuffer, staging.buffer, destination.buffer, 1, &copy);
    const auto submitResult = submitOneShot(device, commandPool, queue, commandBuffer);
    allocator.destroy(staging);
    return submitResult;
}

Halcyon::Result<void> GpuUploader::uploadImage(VkDevice device,
    VkCommandPool commandPool,
    VkQueue queue,
    GpuAllocator& allocator,
    ImageAllocation destination,
    VkExtent3D extent,
    VkFormat format,
    std::span<const std::byte> data,
    VkImageLayout finalLayout)
{
    if (device == VK_NULL_HANDLE || commandPool == VK_NULL_HANDLE || queue == VK_NULL_HANDLE ||
        data.empty() || destination.image == VK_NULL_HANDLE || extent.width == 0 ||
        extent.height == 0 || extent.depth == 0)
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::InvalidArgument, "Invalid image upload parameters"});
    }
    VkBufferCreateInfo stagingInfo{};
    stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagingInfo.size = data.size_bytes();
    stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    const auto stagingResult = allocator.createBuffer(stagingInfo, MemoryUsage::CpuToGpu);
    if (!stagingResult)
    {
        return stagingResult.error();
    }
    const BufferAllocation staging = stagingResult.value();
    const auto writeResult = allocator.writeBuffer(staging, data);
    if (!writeResult)
    {
        allocator.destroy(staging);
        return writeResult;
    }
    const auto commandResult = beginOneShot(device, commandPool);
    if (!commandResult)
    {
        allocator.destroy(staging);
        return commandResult.error();
    }
    const VkCommandBuffer commandBuffer = commandResult.value();
    VkImageMemoryBarrier2 toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    toTransfer.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    toTransfer.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.image = destination.image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &toTransfer;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = extent;
    vkCmdCopyBufferToImage(commandBuffer,
        staging.buffer,
        destination.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &region);

    VkImageMemoryBarrier2 toFinal = toTransfer;
    toFinal.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    toFinal.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toFinal.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    toFinal.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
    toFinal.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toFinal.newLayout = finalLayout;
    dependency.pImageMemoryBarriers = &toFinal;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
    const auto submitResult = submitOneShot(device, commandPool, queue, commandBuffer);
    allocator.destroy(staging);
    (void)format;
    return submitResult;
}

Halcyon::Result<void> GpuUploader::uploadImageWithMips(VkDevice device,
    VkPhysicalDevice physicalDevice,
    VkCommandPool commandPool,
    VkQueue queue,
    GpuAllocator& allocator,
    ImageAllocation destination,
    VkExtent3D extent,
    VkFormat format,
    std::span<const std::byte> data,
    std::uint32_t mipLevels,
    VkImageLayout finalLayout)
{
    if (physicalDevice == VK_NULL_HANDLE || mipLevels == 0)
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::InvalidArgument, "Invalid mipmapped image upload parameters"});
    }
    if (mipLevels == 1)
    {
        return uploadImage(device, commandPool, queue, allocator, destination, extent, format,
            data, finalLayout);
    }
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &properties);
    if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) == 0)
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::Unsupported,
                "linear blit is unavailable for texture format; cannot generate mip chain"});
    }
    if (device == VK_NULL_HANDLE || commandPool == VK_NULL_HANDLE || queue == VK_NULL_HANDLE ||
        data.empty() || destination.image == VK_NULL_HANDLE || extent.width == 0 ||
        extent.height == 0 || extent.depth == 0)
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::InvalidArgument, "Invalid mipmapped image upload parameters"});
    }

    VkBufferCreateInfo stagingInfo{};
    stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagingInfo.size = data.size_bytes();
    stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    const auto stagingResult = allocator.createBuffer(stagingInfo, MemoryUsage::CpuToGpu);
    if (!stagingResult)
    {
        return stagingResult.error();
    }
    const BufferAllocation staging = stagingResult.value();
    const auto writeResult = allocator.writeBuffer(staging, data);
    if (!writeResult)
    {
        allocator.destroy(staging);
        return writeResult;
    }
    const auto commandResult = beginOneShot(device, commandPool);
    if (!commandResult)
    {
        allocator.destroy(staging);
        return commandResult.error();
    }
    const VkCommandBuffer commandBuffer = commandResult.value();
    auto barrier = [&](std::uint32_t level,
                       VkImageLayout oldLayout,
                       VkImageLayout newLayout,
                       VkPipelineStageFlags2 srcStage,
                       VkAccessFlags2 srcAccess,
                       VkPipelineStageFlags2 dstStage,
                       VkAccessFlags2 dstAccess)
    {
        VkImageMemoryBarrier2 imageBarrier{};
        imageBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        imageBarrier.srcStageMask = srcStage;
        imageBarrier.srcAccessMask = srcAccess;
        imageBarrier.dstStageMask = dstStage;
        imageBarrier.dstAccessMask = dstAccess;
        imageBarrier.oldLayout = oldLayout;
        imageBarrier.newLayout = newLayout;
        imageBarrier.image = destination.image;
        imageBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &imageBarrier;
        vkCmdPipelineBarrier2(commandBuffer, &dependency);
    };

    barrier(0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = extent;
    vkCmdCopyBufferToImage(commandBuffer, staging.buffer, destination.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    std::int32_t srcWidth = static_cast<std::int32_t>(extent.width);
    std::int32_t srcHeight = static_cast<std::int32_t>(extent.height);
    for (std::uint32_t level = 1; level < mipLevels; ++level)
    {
        barrier(level - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT);
        barrier(level, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_BLIT_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const std::int32_t dstWidth = std::max(1, srcWidth / 2);
        const std::int32_t dstHeight = std::max(1, srcHeight / 2);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
        blit.srcOffsets[1] = {srcWidth, srcHeight, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        blit.dstOffsets[1] = {dstWidth, dstHeight, 1};
        vkCmdBlitImage(commandBuffer, destination.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            destination.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        barrier(level - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, finalLayout,
            VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
        srcWidth = dstWidth;
        srcHeight = dstHeight;
    }
    barrier(mipLevels - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, finalLayout,
        VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    const auto submitResult = submitOneShot(device, commandPool, queue, commandBuffer);
    allocator.destroy(staging);
    return submitResult;
}

} // namespace Halcyon::Vulkan
