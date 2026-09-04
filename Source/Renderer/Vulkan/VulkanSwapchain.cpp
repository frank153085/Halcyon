#include "VulkanSwapchain.h"

#include "VulkanCommon.h"

#ifndef GLFW_INCLUDE_VULKAN
#define GLFW_INCLUDE_VULKAN
#endif
#include <GLFW/glfw3.h>
#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace Halcyon::Vulkan
{

namespace
{
[[nodiscard]] VkSurfaceFormatKHR chooseSurfaceFormat(
    const std::vector<VkSurfaceFormatKHR>& formats) noexcept
{
    for (const auto& format : formats)
    {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            return format;
        }
    }
    for (const auto& format : formats)
    {
        if (format.format == VK_FORMAT_R8G8B8A8_SRGB &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            return format;
        }
    }
    return formats.front();
}

[[nodiscard]] VkPresentModeKHR choosePresentMode(
    const std::vector<VkPresentModeKHR>& modes) noexcept
{
    for (VkPresentModeKHR mode : modes)
    {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
        {
            return mode;
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

[[nodiscard]] VkCompositeAlphaFlagBitsKHR chooseCompositeAlpha(
    VkCompositeAlphaFlagsKHR supported) noexcept
{
    constexpr std::array<VkCompositeAlphaFlagBitsKHR, 4> choices = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
    };
    for (const auto choice : choices)
    {
        if ((supported & choice) != 0)
        {
            return choice;
        }
    }
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}
} // namespace

Halcyon::Result<void> VulkanSwapchain::initialize(VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkSurfaceKHR surface,
    GLFWwindow* window,
    std::uint32_t graphicsQueueFamily,
    std::uint32_t presentQueueFamily,
    GpuAllocator& allocator,
    VkExtent2D requested) noexcept
{
    physicalDevice_ = physicalDevice;
    device_ = device;
    surface_ = surface;
    window_ = window;
    graphicsQueueFamily_ = graphicsQueueFamily;
    presentQueueFamily_ = presentQueueFamily;
    allocator_ = &allocator;
    requestedExtent = requested;
    deviceLost = false;
    return ok();
}

VkResult VulkanSwapchain::acquire(
    VkSemaphore imageAvailable, std::uint32_t& imageIndex) const noexcept
{
    if (device_ == VK_NULL_HANDLE || swapchain == VK_NULL_HANDLE ||
        imageAvailable == VK_NULL_HANDLE)
    {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return vkAcquireNextImageKHR(device_,
        swapchain,
        std::numeric_limits<std::uint64_t>::max(),
        imageAvailable,
        VK_NULL_HANDLE,
        &imageIndex);
}

VoidResult VulkanSwapchain::createDepthResources(VkExtent2D extent,
    VkImage& outImage,
    ImageAllocation& outAllocation,
    VkImageView& outView,
    VkDeviceSize& outMemorySize)
{
    outImage = VK_NULL_HANDLE;
    outAllocation = {};
    outView = VK_NULL_HANDLE;
    outMemorySize = 0;
    if (extent.width == 0 || extent.height == 0)
    {
        return fail(
            "Cannot create a depth image with a zero extent", Halcyon::ErrorCode::InvalidArgument);
    }

    VkFormatProperties formatProperties{};
    vkGetPhysicalDeviceFormatProperties(physicalDevice_, depthFormat, &formatProperties);
    if ((formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ==
        0)
    {
        return fail("D32_SFLOAT is not supported as an optimal depth attachment",
            Halcyon::ErrorCode::Unsupported);
    }

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = depthFormat;
    imageInfo.extent = VkExtent3D{extent.width, extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    const auto allocationResult = allocator_->createImage(imageInfo, MemoryUsage::GpuOnly);
    if (!allocationResult)
    {
        return allocationResult.error();
    }
    outAllocation = allocationResult.value();
    outImage = outAllocation.image;

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = outImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = depthFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    const VkResult result = vkCreateImageView(device_, &viewInfo, nullptr, &outView);
    if (result != VK_SUCCESS)
    {
        allocator_->destroy(outAllocation);
        outView = VK_NULL_HANDLE;
        outAllocation = {};
        outImage = VK_NULL_HANDLE;
        return fail(vkFailure("vkCreateImageView(depth)", result));
    }
    outMemorySize = outAllocation.size;
    return ok();
}

VoidResult VulkanSwapchain::create()
{
    const VkPhysicalDevice physicalDevice = physicalDevice_;
    const VkDevice device = device_;
    const VkSurfaceKHR surface = surface_;
    GLFWwindow* window = window_;
    const std::uint32_t graphicsQueueFamily = graphicsQueueFamily_;
    const std::uint32_t presentQueueFamily = presentQueueFamily_;
    GpuAllocator& gpuAllocator = *allocator_;
    VkSurfaceCapabilitiesKHR capabilities{};
    VkResult result =
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &capabilities);
    if (result != VK_SUCCESS)
    {
        return fail(vkFailure("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", result));
    }
    if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0)
    {
        return fail("The surface does not support color-attachment swapchain images",
            Halcyon::ErrorCode::Unsupported);
    }

    std::uint32_t formatCount = 0;
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr);
    if (result != VK_SUCCESS || formatCount == 0)
    {
        return fail(result != VK_SUCCESS ? vkFailure("vkGetPhysicalDeviceSurfaceFormatsKHR", result)
                                         : "The surface exposes no formats");
    }
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    result =
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, formats.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE)
    {
        return fail(vkFailure("vkGetPhysicalDeviceSurfaceFormatsKHR", result));
    }
    formats.resize(formatCount);

    std::uint32_t presentModeCount = 0;
    result = vkGetPhysicalDeviceSurfacePresentModesKHR(
        physicalDevice, surface, &presentModeCount, nullptr);
    if (result != VK_SUCCESS || presentModeCount == 0)
    {
        return fail(result != VK_SUCCESS
                        ? vkFailure("vkGetPhysicalDeviceSurfacePresentModesKHR", result)
                        : "The surface exposes no present modes");
    }
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    result = vkGetPhysicalDeviceSurfacePresentModesKHR(
        physicalDevice, surface, &presentModeCount, presentModes.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE)
    {
        return fail(vkFailure("vkGetPhysicalDeviceSurfacePresentModesKHR", result));
    }
    presentModes.resize(presentModeCount);

    VkExtent2D extent{};
    if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max())
    {
        extent = capabilities.currentExtent;
    }
    else
    {
        int framebufferWidth = 0;
        int framebufferHeight = 0;
        glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
        const std::uint32_t requestedWidth = framebufferWidth > 0
                                                 ? static_cast<std::uint32_t>(framebufferWidth)
                                                 : requestedExtent.width;
        const std::uint32_t requestedHeight = framebufferHeight > 0
                                                  ? static_cast<std::uint32_t>(framebufferHeight)
                                                  : requestedExtent.height;
        extent.width = std::clamp(
            requestedWidth, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height = std::clamp(requestedHeight,
            capabilities.minImageExtent.height,
            capabilities.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0)
    {
        // Minimized windows have a zero framebuffer.  Leave the existing
        // swapchain intact and let render() retry after restoration.
        return ok();
    }

    const VkSurfaceFormatKHR surfaceFormat = chooseSurfaceFormat(formats);
    const VkPresentModeKHR presentMode = choosePresentMode(presentModes);
    std::uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount)
    {
        imageCount = capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0)
    {
        return fail("The surface does not support transfer-source swapchain images",
            Halcyon::ErrorCode::Unsupported);
    }
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const std::array<std::uint32_t, 2> queueFamilies = {graphicsQueueFamily, presentQueueFamily};
    if (graphicsQueueFamily != presentQueueFamily)
    {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilies.data();
    }
    else
    {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = chooseCompositeAlpha(capabilities.supportedCompositeAlpha);
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = swapchain;

    VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
    result = vkCreateSwapchainKHR(device, &createInfo, nullptr, &newSwapchain);
    if (result != VK_SUCCESS)
    {
        return fail(vkFailure("vkCreateSwapchainKHR", result));
    }

    std::uint32_t imageCountReturned = 0;
    result = vkGetSwapchainImagesKHR(device, newSwapchain, &imageCountReturned, nullptr);
    if (result != VK_SUCCESS || imageCountReturned == 0)
    {
        vkDestroySwapchainKHR(device, newSwapchain, nullptr);
        return fail(result != VK_SUCCESS ? vkFailure("vkGetSwapchainImagesKHR", result)
                                         : "vkGetSwapchainImagesKHR returned no images");
    }
    std::vector<VkImage> newImages(imageCountReturned);
    result = vkGetSwapchainImagesKHR(device, newSwapchain, &imageCountReturned, newImages.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE)
    {
        vkDestroySwapchainKHR(device, newSwapchain, nullptr);
        return fail(vkFailure("vkGetSwapchainImagesKHR", result));
    }
    newImages.resize(imageCountReturned);
    std::vector<VkImageView> newViews;
    newViews.reserve(newImages.size());
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = surfaceFormat.format;
    viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    for (VkImage image : newImages)
    {
        viewInfo.image = image;
        VkImageView view = VK_NULL_HANDLE;
        result = vkCreateImageView(device, &viewInfo, nullptr, &view);
        if (result != VK_SUCCESS)
        {
            for (VkImageView created : newViews)
            {
                vkDestroyImageView(device, created, nullptr);
            }
            vkDestroySwapchainKHR(device, newSwapchain, nullptr);
            return fail(vkFailure("vkCreateImageView", result));
        }
        newViews.push_back(view);
    }

    std::vector<VkSemaphore> newPresentSemaphores;
    newPresentSemaphores.reserve(newImages.size());
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (std::size_t i = 0; i < newImages.size(); ++i)
    {
        VkSemaphore semaphore = VK_NULL_HANDLE;
        result = vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore);
        if (result != VK_SUCCESS)
        {
            for (VkSemaphore created : newPresentSemaphores)
            {
                vkDestroySemaphore(device, created, nullptr);
            }
            for (VkImageView created : newViews)
            {
                vkDestroyImageView(device, created, nullptr);
            }
            vkDestroySwapchainKHR(device, newSwapchain, nullptr);
            return fail(vkFailure("vkCreateSemaphore(presentReady)", result));
        }
        newPresentSemaphores.push_back(semaphore);
    }

    VkImage newDepthImage = VK_NULL_HANDLE;
    ImageAllocation newDepthAllocation{};
    VkImageView newDepthView = VK_NULL_HANDLE;
    VkDeviceSize newDepthMemorySize = 0;
    const VoidResult depthResult = createDepthResources(
        extent, newDepthImage, newDepthAllocation, newDepthView, newDepthMemorySize);
    if (!depthResult)
    {
        for (VkSemaphore created : newPresentSemaphores)
        {
            vkDestroySemaphore(device, created, nullptr);
        }
        for (VkImageView created : newViews)
        {
            vkDestroyImageView(device, created, nullptr);
        }
        vkDestroySwapchainKHR(device, newSwapchain, nullptr);
        return depthResult;
    }

    // Prepare the initialization bitmap before detaching any old
    // swapchain state.  vector<bool>::assign() may allocate; moving old
    // Vulkan handles first would make a bad_alloc leave them detached
    // from the renderer.  The commit below uses noexcept swaps.
    std::vector<bool> newImageInitialized(newImages.size(), false);

    // Commit the new swapchain only after all image views and synchronization
    // objects have been created successfully. M3 pipelines are rebuilt by the
    // renderer immediately after this state transition.
    const VkSwapchainKHR oldSwapchain = std::exchange(swapchain, newSwapchain);
    std::vector<VkImageView> oldViews;
    oldViews.swap(swapchainImageViews);
    std::vector<VkSemaphore> oldPresentSemaphores;
    oldPresentSemaphores.swap(presentReadySemaphores);
    swapchainImages.swap(newImages);
    swapchainImageViews.swap(newViews);
    presentReadySemaphores.swap(newPresentSemaphores);
    swapchainImageInitialized.swap(newImageInitialized);
    swapchainFormat = surfaceFormat.format;
    swapchainColorSpace = surfaceFormat.colorSpace;
    swapchainExtent = extent;
    (void)std::exchange(depthImage, newDepthImage);
    const ImageAllocation oldDepthAllocation = std::exchange(depthAllocation, newDepthAllocation);
    const VkImageView oldDepthView = std::exchange(depthImageView, newDepthView);
    depthMemorySize = newDepthMemorySize;
    depthImageInitialized = false;
    if (device != VK_NULL_HANDLE)
    {
        for (VkImageView view : oldViews)
        {
            if (view != VK_NULL_HANDLE)
            {
                vkDestroyImageView(device, view, nullptr);
            }
        }
        for (VkSemaphore semaphore : oldPresentSemaphores)
        {
            if (semaphore != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(device, semaphore, nullptr);
            }
        }
        if (oldSwapchain != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(device, oldSwapchain, nullptr);
        }
    }
    if (device != VK_NULL_HANDLE)
    {
        if (oldDepthView != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device, oldDepthView, nullptr);
        }
        gpuAllocator.destroy(oldDepthAllocation);
    }
    framebufferResized = false;
    return ok();
}

VoidResult VulkanSwapchain::recreate()
{
    const VkDevice device = device_;
    const VkSurfaceKHR surface = surface_;
    GLFWwindow* window = window_;
    if (device == VK_NULL_HANDLE || surface == VK_NULL_HANDLE)
    {
        return fail("Cannot recreate swapchain before device/surface creation");
    }
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    if (width <= 0 || height <= 0)
    {
        return ok();
    }
    const VkResult waitResult = vkDeviceWaitIdle(device);
    if (waitResult != VK_SUCCESS)
    {
        if (waitResult == VK_ERROR_DEVICE_LOST)
        {
            deviceLost = true;
        }
        return fail(vkFailure("vkDeviceWaitIdle(recreateSwapchain)", waitResult));
    }
    requestedExtent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
    return create();
}

void VulkanSwapchain::destroyDepthResources() noexcept
{
    if (device_ != VK_NULL_HANDLE)
    {
        if (depthImageView != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device_, depthImageView, nullptr);
        }
        allocator_->destroy(depthAllocation);
    }
    depthImageView = VK_NULL_HANDLE;
    depthImage = VK_NULL_HANDLE;
    depthAllocation = {};
    depthMemorySize = 0;
    depthImageInitialized = false;
}

void VulkanSwapchain::cleanup() noexcept
{
    if (device_ != VK_NULL_HANDLE)
    {
        destroyDepthResources();
        for (VkImageView view : swapchainImageViews)
        {
            if (view != VK_NULL_HANDLE)
            {
                vkDestroyImageView(device_, view, nullptr);
            }
        }
        for (VkSemaphore semaphore : presentReadySemaphores)
        {
            if (semaphore != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(device_, semaphore, nullptr);
            }
        }
    }
    swapchainImageViews.clear();
    presentReadySemaphores.clear();
    swapchainImages.clear();
    swapchainImageInitialized.clear();
    if (device_ != VK_NULL_HANDLE && swapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(device_, swapchain, nullptr);
    }
    swapchain = VK_NULL_HANDLE;
    swapchainFormat = VK_FORMAT_UNDEFINED;
    swapchainColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swapchainExtent = {};
}

} // namespace Halcyon::Vulkan
