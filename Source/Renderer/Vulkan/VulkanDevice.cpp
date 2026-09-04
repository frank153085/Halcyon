#include "VulkanDevice.h"

#include "VulkanCommon.h"

#ifndef GLFW_INCLUDE_VULKAN
#define GLFW_INCLUDE_VULKAN
#endif
#include <GLFW/glfw3.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Halcyon::Vulkan
{
namespace
{
constexpr std::uint32_t kRequiredApiVersion = VK_API_VERSION_1_3;

template <typename T>
[[nodiscard]] bool hasName(const std::vector<T>& properties, const char* requested) noexcept
{
    for (const auto& property : properties)
    {
        if (std::strcmp(property.extensionName, requested) == 0)
        {
            return true;
        }
    }
    return false;
}

template <typename T>
[[nodiscard]] bool hasLayerName(const std::vector<T>& properties, const char* requested) noexcept
{
    for (const auto& property : properties)
    {
        if (std::strcmp(property.layerName, requested) == 0)
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::vector<const char*> uniqueNames(const std::vector<const char*>& names)
{
    std::vector<const char*> result;
    result.reserve(names.size());
    for (const char* name : names)
    {
        bool duplicate = false;
        for (const char* existing : result)
        {
            if (std::strcmp(existing, name) == 0)
            {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
        {
            result.push_back(name);
        }
    }
    return result;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void* /*userData*/)
{
    const char* label = "INFO";
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
    {
        label = "ERROR";
    }
    else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0)
    {
        label = "WARN";
    }
    else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) != 0)
    {
        label = "VERBOSE";
    }
    std::fprintf(stderr,
        "[Halcyon/Vulkan %s] %s\n",
        label,
        callbackData != nullptr && callbackData->pMessage != nullptr ? callbackData->pMessage
                                                                     : "<no message>");
    return VK_FALSE;
}


struct QueueSelection
{
    std::uint32_t graphics = VK_QUEUE_FAMILY_IGNORED;
    std::uint32_t present = VK_QUEUE_FAMILY_IGNORED;

    [[nodiscard]] bool valid() const noexcept
    {
        return graphics != VK_QUEUE_FAMILY_IGNORED && present != VK_QUEUE_FAMILY_IGNORED;
    }
};

struct DeviceCandidate
{
    VkPhysicalDevice handle = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceVulkan12Features features12{};
    VkPhysicalDeviceVulkan13Features features13{};
    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR barycentric{};
    VkPhysicalDeviceRayQueryFeaturesKHR rayQuery{};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructure{};
    VkPhysicalDeviceFeatures coreFeatures{};
    QueueSelection queues{};
    bool hasBarycentricExtension = false;
    bool hasRayQueryExtensions = false;
    bool rayQuerySupported = false;
    bool barycentricSupported = false;
    std::uint64_t deviceLocalBytes = 0;
    int score = std::numeric_limits<int>::min();
};

[[nodiscard]] QueueSelection findQueues(VkPhysicalDevice device, VkSurfaceKHR surface)
{
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    if (count == 0)
    {
        return {};
    }
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

    QueueSelection selection{};
    // Prefer a family that can do both operations.  A single queue keeps the
    // first slice simple and avoids ownership transfers for the swapchain.
    for (std::uint32_t i = 0; i < count; ++i)
    {
        if (families[i].queueCount == 0 ||
            (families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) !=
                (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
        {
            continue;
        }
        VkBool32 present = VK_FALSE;
        if (vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &present) != VK_SUCCESS ||
            present == VK_FALSE)
        {
            continue;
        }
        selection.graphics = i;
        selection.present = i;
        return selection;
    }

    for (std::uint32_t i = 0; i < count; ++i)
    {
        if (families[i].queueCount > 0 &&
            (families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
        {
            selection.graphics = i;
            break;
        }
    }
    for (std::uint32_t i = 0; i < count; ++i)
    {
        if (families[i].queueCount == 0)
        {
            continue;
        }
        VkBool32 present = VK_FALSE;
        if (vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &present) == VK_SUCCESS &&
            present != VK_FALSE)
        {
            selection.present = i;
            break;
        }
    }
    return selection;
}

[[nodiscard]] bool queryDeviceExtensions(
    VkPhysicalDevice device, std::vector<VkExtensionProperties>& out)
{
    std::uint32_t count = 0;
    VkResult result = vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    if (result != VK_SUCCESS)
    {
        return false;
    }
    out.resize(count);
    if (count != 0)
    {
        result = vkEnumerateDeviceExtensionProperties(device, nullptr, &count, out.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE)
        {
            out.clear();
            return false;
        }
        out.resize(count);
    }
    return true;
}

[[nodiscard]] bool queryCandidateFeatures(
    DeviceCandidate& candidate, const std::vector<VkExtensionProperties>& extensions)
{
    candidate.features12 = {};
    candidate.features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    candidate.features13 = {};
    candidate.features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &candidate.features12;
    candidate.features12.pNext = &candidate.features13;

    candidate.hasBarycentricExtension =
        hasName(extensions, VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);
    candidate.hasRayQueryExtensions =
        hasName(extensions, VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
        hasName(extensions, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
        hasName(extensions, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);

    if (candidate.hasBarycentricExtension)
    {
        candidate.barycentric = {};
        candidate.barycentric.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR;
        candidate.features13.pNext = &candidate.barycentric;
    }
    if (candidate.hasRayQueryExtensions)
    {
        candidate.rayQuery = {};
        candidate.rayQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
        candidate.accelerationStructure = {};
        candidate.accelerationStructure.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        if (candidate.hasBarycentricExtension)
        {
            candidate.barycentric.pNext = &candidate.rayQuery;
        }
        else
        {
            candidate.features13.pNext = &candidate.rayQuery;
        }
        candidate.rayQuery.pNext = &candidate.accelerationStructure;
    }

    vkGetPhysicalDeviceFeatures2(candidate.handle, &features2);
    candidate.coreFeatures = features2.features;
    candidate.barycentricSupported =
        candidate.hasBarycentricExtension && candidate.barycentric.fragmentShaderBarycentric;
    candidate.rayQuerySupported = candidate.hasRayQueryExtensions && candidate.rayQuery.rayQuery &&
                                  candidate.accelerationStructure.accelerationStructure &&
                                  candidate.features12.bufferDeviceAddress;
    return true;
}

[[nodiscard]] int deviceTypeScore(VkPhysicalDeviceType type) noexcept
{
    switch (type)
    {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            return 10000;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            return 5000;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            return 2500;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            return 1000;
        default:
            return 0;
    }
}





} // namespace

Halcyon::Result<void> VulkanDevice::initialize(
    GLFWwindow* window, const RendererConfig& config)
{
    cleanup();
    window_ = window;
    config_ = config;
    auto result = createInstance();
    if (!result) return result;
    result = createSurface();
    if (!result) { cleanup(); return result; }
    result = pickPhysicalDevice();
    if (!result) { cleanup(); return result; }
    result = createDevice();
    if (!result) { cleanup(); return result; }
    return ok();
}

VoidResult VulkanDevice::createInstance()
    {
    const RendererConfig& config = config_;
        std::uint32_t loaderVersion = VK_API_VERSION_1_0;
        // vkEnumerateInstanceVersion was introduced in Vulkan 1.1.  The
        // loader linked by the Vulkan SDK always exposes it, but accepting a
        // graceful error here makes startup diagnostics clearer on old hosts.
        VkResult result = vkEnumerateInstanceVersion(&loaderVersion);
        if (result != VK_SUCCESS && result != VK_INCOMPLETE)
        {
            return fail(vkFailure("vkEnumerateInstanceVersion", result));
        }
        capabilities.instanceApiVersion = loaderVersion;
        if (VK_VERSION_MAJOR(loaderVersion) < 1 ||
            (VK_VERSION_MAJOR(loaderVersion) == 1 && VK_VERSION_MINOR(loaderVersion) < 3))
        {
            return fail("A Vulkan 1.3 loader is required by the Halcyon M1 renderer");
        }

        std::uint32_t extensionCount = 0;
        result = vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);
        if (result != VK_SUCCESS)
        {
            return fail(vkFailure("vkEnumerateInstanceExtensionProperties", result));
        }
        std::vector<VkExtensionProperties> availableExtensions(extensionCount);
        if (extensionCount != 0)
        {
            result = vkEnumerateInstanceExtensionProperties(
                nullptr, &extensionCount, availableExtensions.data());
            if (result != VK_SUCCESS && result != VK_INCOMPLETE)
            {
                return fail(vkFailure("vkEnumerateInstanceExtensionProperties", result));
            }
            availableExtensions.resize(extensionCount);
        }

        std::uint32_t layerCount = 0;
        result = vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        if (result != VK_SUCCESS)
        {
            return fail(vkFailure("vkEnumerateInstanceLayerProperties", result));
        }
        std::vector<VkLayerProperties> availableLayers(layerCount);
        if (layerCount != 0)
        {
            result = vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());
            if (result != VK_SUCCESS && result != VK_INCOMPLETE)
            {
                return fail(vkFailure("vkEnumerateInstanceLayerProperties", result));
            }
            availableLayers.resize(layerCount);
        }

        std::uint32_t glfwExtensionCount = 0;
        const char* const* glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
        if (glfwExtensions == nullptr || glfwExtensionCount == 0)
        {
            return fail("GLFW did not provide the required Vulkan instance extensions");
        }
        std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);

        const bool debugUtilsAvailable =
            hasName(availableExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        if (config.enableValidation && debugUtilsAvailable)
        {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            capabilities.debugUtils = true;
        }
        extensions = uniqueNames(extensions);
        for (const char* extension : extensions)
        {
            if (!hasName(availableExtensions, extension))
            {
                return fail(std::string("Required instance extension is not "
                                        "available: ") +
                            extension);
            }
        }

        std::vector<const char*> layers;
        const bool validationAvailable =
            hasLayerName(availableLayers, "VK_LAYER_KHRONOS_validation");
        if (config.enableValidation && validationAvailable)
        {
            layers.push_back("VK_LAYER_KHRONOS_validation");
            capabilities.validationEnabled = true;
        }

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName =
            config.applicationName != nullptr ? config.applicationName : "Halcyon";
        appInfo.applicationVersion = config.applicationVersion;
        appInfo.pEngineName = "Halcyon Engine";
        appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        appInfo.apiVersion = kRequiredApiVersion;

        VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
        debugCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        debugCreateInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debugCreateInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debugCreateInfo.pfnUserCallback = debugCallback;

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
        createInfo.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
        createInfo.ppEnabledLayerNames = layers.data();
        if (capabilities.debugUtils)
        {
            createInfo.pNext = &debugCreateInfo;
        }

        result = vkCreateInstance(&createInfo, nullptr, &instance);
        if (result != VK_SUCCESS)
        {
            return fail(vkFailure("vkCreateInstance", result));
        }

        if (capabilities.debugUtils)
        {
            const auto createDebugFn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
            if (createDebugFn != nullptr)
            {
                result = createDebugFn(instance, &debugCreateInfo, nullptr, &debugMessenger);
                if (result != VK_SUCCESS)
                {
                    // Debug output is useful but must never make a renderer
                    // unusable when a loader advertises a partial extension.
                    debugMessenger = VK_NULL_HANDLE;
                    capabilities.debugUtils = false;
                }
            }
            else
            {
                capabilities.debugUtils = false;
            }
        }
        return ok();
    }

    

VoidResult VulkanDevice::createSurface()
    {
        GLFWwindow* window = window_;
        if (window == nullptr)
        {
            return fail("Renderer::initialize received a null GLFWwindow");
        }
        VkSurfaceKHR createdSurface = VK_NULL_HANDLE;
        const VkResult result = glfwCreateWindowSurface(instance, window, nullptr, &createdSurface);
        if (result != VK_SUCCESS)
        {
            return fail(vkFailure("glfwCreateWindowSurface", result));
        }
        surface = createdSurface;
        return ok();
    }

    

VoidResult VulkanDevice::pickPhysicalDevice()
    {
        const RendererConfig& config = config_;
        std::uint32_t count = 0;
        VkResult result = vkEnumeratePhysicalDevices(instance, &count, nullptr);
        if (result != VK_SUCCESS)
        {
            return fail(vkFailure("vkEnumeratePhysicalDevices", result));
        }
        if (count == 0)
        {
            return fail("No Vulkan physical device was found");
        }
        std::vector<VkPhysicalDevice> devices(count);
        result = vkEnumeratePhysicalDevices(instance, &count, devices.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE)
        {
            return fail(vkFailure("vkEnumeratePhysicalDevices", result));
        }
        devices.resize(count);

        DeviceCandidate best{};
        std::string lastUnsupportedReason = "no physical devices were evaluated";
        for (VkPhysicalDevice candidateHandle : devices)
        {
            DeviceCandidate candidate{};
            candidate.handle = candidateHandle;
            vkGetPhysicalDeviceProperties(candidate.handle, &candidate.properties);
            if (candidate.properties.apiVersion < kRequiredApiVersion)
            {
                lastUnsupportedReason = "Vulkan 1.3 is unavailable";
                continue;
            }

            std::vector<VkExtensionProperties> extensions;
            if (!queryDeviceExtensions(candidate.handle, extensions) ||
                !hasName(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
            {
                lastUnsupportedReason = "VK_KHR_swapchain is unavailable";
                continue;
            }
            candidate.queues = findQueues(candidate.handle, surface);
            if (!candidate.queues.valid())
            {
                lastUnsupportedReason =
                    "no graphics+compute queue and presentation queue combination is available";
                continue;
            }
            if (!queryCandidateFeatures(candidate, extensions))
            {
                lastUnsupportedReason = "Vulkan feature query failed";
                continue;
            }
            if (config.rayQuery == FeatureMode::Required && !candidate.rayQuerySupported)
            {
                lastUnsupportedReason = "required ray-query features are unavailable";
                continue;
            }
            // Cluster build uses RWStructuredBuffer atomics.  Vulkan exposes
            // the corresponding capability through the core
            // fragmentStoresAndAtomics feature bit; require it up front so a
            // device can never enter a graph path whose atomic writes are
            // silently unsupported.
            if (candidate.features13.dynamicRendering == VK_FALSE ||
                candidate.features13.synchronization2 == VK_FALSE ||
                candidate.features13.shaderDemoteToHelperInvocation == VK_FALSE ||
                candidate.features12.timelineSemaphore == VK_FALSE ||
                candidate.coreFeatures.fragmentStoresAndAtomics == VK_FALSE)
            {
                if (candidate.features13.dynamicRendering == VK_FALSE)
                    lastUnsupportedReason = "dynamicRendering is unavailable";
                else if (candidate.features13.synchronization2 == VK_FALSE)
                    lastUnsupportedReason = "synchronization2 is unavailable";
                else if (candidate.features12.timelineSemaphore == VK_FALSE)
                    lastUnsupportedReason = "timelineSemaphore is unavailable";
                else if (candidate.features13.shaderDemoteToHelperInvocation == VK_FALSE)
                    lastUnsupportedReason = "shaderDemoteToHelperInvocation is unavailable";
                else
                    lastUnsupportedReason = "fragmentStoresAndAtomics is unavailable";
                continue;
            }
            if (VK_VERSION_MINOR(candidate.properties.apiVersion) < 3)
            {
                lastUnsupportedReason = "device API version is below Vulkan 1.3";
                continue;
            }
            const auto supportsFormat = [&](VkFormat format, VkFormatFeatureFlags required)
            {
                VkFormatProperties properties{};
                vkGetPhysicalDeviceFormatProperties(candidate.handle, format, &properties);
                return (properties.optimalTilingFeatures & required) == required;
            };
            const auto requireFormat = [&](VkFormat format, VkFormatFeatureFlags required,
                                           const char* name)
            {
                if (supportsFormat(format, required))
                    return true;
                lastUnsupportedReason = std::string(name) +
                    " lacks required optimal-tiling format features";
                return false;
            };
            if (!requireFormat(VK_FORMAT_R8G8B8A8_SRGB,
                    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
                    "VK_FORMAT_R8G8B8A8_SRGB") ||
                !requireFormat(VK_FORMAT_R8G8B8A8_UNORM,
                    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
                    "VK_FORMAT_R8G8B8A8_UNORM") ||
                !requireFormat(VK_FORMAT_R16G16B16A16_SFLOAT,
                    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
                    "VK_FORMAT_R16G16B16A16_SFLOAT") ||
                !requireFormat(VK_FORMAT_R16G16_SFLOAT,
                    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
                    "VK_FORMAT_R16G16_SFLOAT") ||
                !requireFormat(VK_FORMAT_D32_SFLOAT,
                    VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
                    "VK_FORMAT_D32_SFLOAT"))
            {
                continue;
            }
            if (candidate.properties.limits.maxDescriptorSetSampledImages < 8 ||
                candidate.properties.limits.maxPerStageDescriptorSampledImages < 8 ||
                candidate.properties.limits.maxDescriptorSetStorageImages < 1 ||
                candidate.properties.limits.maxPerStageDescriptorStorageImages < 1 ||
                candidate.properties.limits.maxDescriptorSetStorageBuffers < 4 ||
                candidate.properties.limits.maxPerStageDescriptorStorageBuffers < 4 ||
                candidate.properties.limits.maxDescriptorSetSamplers < 1 ||
                candidate.properties.limits.maxPerStageDescriptorSamplers < 1 ||
                candidate.properties.limits.maxDescriptorSetUniformBuffers < 1 ||
                candidate.properties.limits.maxPerStageDescriptorUniformBuffers < 1 ||
                candidate.properties.limits.maxImageArrayLayers < 4 ||
                candidate.properties.limits.maxPushConstantsSize < 256)
            {
                lastUnsupportedReason =
                    "descriptor, image-array-layer, or 256-byte push-constant limits are insufficient";
                continue;
            }

            VkFormatProperties depthProperties{};
            vkGetPhysicalDeviceFormatProperties(
                candidate.handle, VK_FORMAT_D32_SFLOAT, &depthProperties);
            if ((depthProperties.optimalTilingFeatures &
                    VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0)
            {
                // M1 intentionally has one well-defined depth format.  A
                // later capability tier can add D24/D32S8 fallbacks without
                // silently changing reversed-Z precision here.
                lastUnsupportedReason = "VK_FORMAT_D32_SFLOAT depth attachments are unavailable";
                continue;
            }

            std::uint32_t surfaceFormatCount = 0;
            std::uint32_t presentModeCount = 0;
            if (vkGetPhysicalDeviceSurfaceFormatsKHR(
                    candidate.handle, surface, &surfaceFormatCount, nullptr) != VK_SUCCESS ||
                vkGetPhysicalDeviceSurfacePresentModesKHR(
                    candidate.handle, surface, &presentModeCount, nullptr) != VK_SUCCESS ||
                surfaceFormatCount == 0 || presentModeCount == 0)
            {
                lastUnsupportedReason = "the presentation surface has no usable format or present mode";
                continue;
            }

            VkPhysicalDeviceMemoryProperties memory{};
            vkGetPhysicalDeviceMemoryProperties(candidate.handle, &memory);
            for (std::uint32_t i = 0; i < memory.memoryHeapCount; ++i)
            {
                if ((memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0)
                {
                    candidate.deviceLocalBytes += memory.memoryHeaps[i].size;
                }
            }
            candidate.score = deviceTypeScore(candidate.properties.deviceType) +
                              static_cast<int>(candidate.properties.limits.maxImageDimension2D);
            if (candidate.score > best.score)
            {
                best = candidate;
            }
        }

        if (best.handle == VK_NULL_HANDLE)
        {
            return fail("No Vulkan 1.3 device with graphics/present queues, swapchain, "
                "dynamic rendering, synchronization2, timeline semaphores, storage-buffer atomics, required "
                        "MRT/storage formats, and descriptor limits was found. Last rejection: " +
                    lastUnsupportedReason,
                Halcyon::ErrorCode::Unsupported);
        }

        physicalDevice = best.handle;
        physicalProperties = best.properties;
        graphicsQueueFamily = best.queues.graphics;
        presentQueueFamily = best.queues.present;
        deviceLocalBytes = best.deviceLocalBytes;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);

        capabilities.deviceApiVersion = physicalProperties.apiVersion;
        capabilities.deviceName = physicalProperties.deviceName;
        capabilities.vendorId = physicalProperties.vendorID;
        capabilities.deviceId = physicalProperties.deviceID;
        capabilities.driverVersion = physicalProperties.driverVersion;
        capabilities.deviceLocalMemoryBytes = best.deviceLocalBytes;
        capabilities.dynamicRendering = best.features13.dynamicRendering != VK_FALSE;
        capabilities.synchronization2 = best.features13.synchronization2 != VK_FALSE;
        capabilities.timelineSemaphore = best.features12.timelineSemaphore != VK_FALSE;
        capabilities.descriptorIndexing = best.features12.descriptorIndexing != VK_FALSE;
        capabilities.bufferDeviceAddress = best.features12.bufferDeviceAddress != VK_FALSE;
        capabilities.indirectCount = best.features12.drawIndirectCount != VK_FALSE;
        capabilities.fragmentBarycentric = best.barycentricSupported;
        capabilities.rayQuery = best.rayQuerySupported;
        capabilities.depthD32 = true;
        capabilities.reversedZ = true;
        capabilities.swapchain = true;
        capabilities.graphicsQueueFamily = graphicsQueueFamily;
        capabilities.presentQueueFamily = presentQueueFamily;

        if (config.rayQuery == FeatureMode::Required && !capabilities.rayQuery)
        {
            return fail(
                "Ray Query was required but the selected Vulkan device does not support it");
        }
        return ok();
    }

    

VoidResult VulkanDevice::createDevice()
    {
        const RendererConfig& config = config_;
        std::set<std::uint32_t> uniqueQueueFamilies = {graphicsQueueFamily, presentQueueFamily};
        const float queuePriority = 1.0f;
        std::vector<VkDeviceQueueCreateInfo> queueInfos;
        queueInfos.reserve(uniqueQueueFamilies.size());
        for (std::uint32_t family : uniqueQueueFamilies)
        {
            VkDeviceQueueCreateInfo queueInfo{};
            queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueInfo.queueFamilyIndex = family;
            queueInfo.queueCount = 1;
            queueInfo.pQueuePriorities = &queuePriority;
            queueInfos.push_back(queueInfo);
        }

        std::uint32_t extensionCount = 0;
        VkResult result =
            vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);
        if (result != VK_SUCCESS)
        {
            return fail(vkFailure("vkEnumerateDeviceExtensionProperties", result));
        }
        std::vector<VkExtensionProperties> availableExtensions(extensionCount);
        if (extensionCount != 0)
        {
            result = vkEnumerateDeviceExtensionProperties(
                physicalDevice, nullptr, &extensionCount, availableExtensions.data());
            if (result != VK_SUCCESS && result != VK_INCOMPLETE)
            {
                return fail(vkFailure("vkEnumerateDeviceExtensionProperties", result));
            }
            availableExtensions.resize(extensionCount);
        }

        std::vector<const char*> deviceExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        if (capabilities.fragmentBarycentric)
        {
            deviceExtensions.push_back(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);
        }
        const std::size_t baseExtensionCount = deviceExtensions.size();
        const bool canUseRayQuery = capabilities.rayQuery && config.rayQuery != FeatureMode::Disabled;
        if (canUseRayQuery)
        {
            deviceExtensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
            deviceExtensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
            deviceExtensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        }
        for (const char* extension : deviceExtensions)
        {
            if (!hasName(availableExtensions, extension))
            {
                if (canUseRayQuery && config.rayQuery == FeatureMode::Auto)
                {
                    // A driver can expose a promoted feature without all of
                    // the extension aliases.  Fall back to the Base tier.
                    deviceExtensions.resize(baseExtensionCount);
                    break;
                }
                return fail(std::string("Required device extension is not "
                                        "available: ") +
                            extension);
            }
        }

        VkPhysicalDeviceFeatures2 enabledFeatures{};
        enabledFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        VkPhysicalDeviceVulkan12Features enabled12{};
        enabled12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        enabled12.timelineSemaphore = VK_TRUE;
        enabled12.descriptorIndexing = capabilities.descriptorIndexing ? VK_TRUE : VK_FALSE;
        enabled12.bufferDeviceAddress = capabilities.bufferDeviceAddress ? VK_TRUE : VK_FALSE;
        enabled12.drawIndirectCount = capabilities.indirectCount ? VK_TRUE : VK_FALSE;
        VkPhysicalDeviceVulkan13Features enabled13{};
        enabled13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        enabled13.dynamicRendering = VK_TRUE;
        enabled13.synchronization2 = VK_TRUE;
        // Alpha-mask materials use HLSL clip(), which DXC lowers to the
        // SPIR-V DemoteToHelperInvocation capability.  Enable the Vulkan
        // feature explicitly and reject devices that cannot provide it;
        // silently creating modules without this feature produces validation
        // errors and undefined alpha-test behaviour.
        enabled13.shaderDemoteToHelperInvocation = VK_TRUE;
        enabledFeatures.features.fragmentStoresAndAtomics = VK_TRUE;
        enabledFeatures.pNext = &enabled12;
        enabled12.pNext = &enabled13;

        VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR enabledBarycentric{};
        if (capabilities.fragmentBarycentric)
        {
            enabledBarycentric.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR;
            enabledBarycentric.fragmentShaderBarycentric = VK_TRUE;
            enabled13.pNext = &enabledBarycentric;
        }

        VkPhysicalDeviceRayQueryFeaturesKHR enabledRay{};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR enabledAcceleration{};
        if (canUseRayQuery && deviceExtensions.size() >= baseExtensionCount + 3u)
        {
            enabledRay.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
            enabledRay.rayQuery = VK_TRUE;
            enabledAcceleration.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
            enabledAcceleration.accelerationStructure = VK_TRUE;
            if (capabilities.fragmentBarycentric)
            {
                enabledBarycentric.pNext = &enabledRay;
            }
            else
            {
                enabled13.pNext = &enabledRay;
            }
            enabledRay.pNext = &enabledAcceleration;
            rayQueryEnabled = true;
        }

        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.pNext = &enabledFeatures;
        createInfo.queueCreateInfoCount = static_cast<std::uint32_t>(queueInfos.size());
        createInfo.pQueueCreateInfos = queueInfos.data();
        createInfo.enabledExtensionCount = static_cast<std::uint32_t>(deviceExtensions.size());
        createInfo.ppEnabledExtensionNames = deviceExtensions.data();

        result = vkCreateDevice(physicalDevice, &createInfo, nullptr, &device);
        if (result != VK_SUCCESS && rayQueryEnabled && config.rayQuery == FeatureMode::Auto)
        {
            // Retry the Base feature set if an optional RT extension fails
            // device creation.  This is intentionally only done for Auto;
            // Required surfaces the original error to the caller.
            device = VK_NULL_HANDLE;
            rayQueryEnabled = false;
            if (capabilities.fragmentBarycentric)
            {
                enabledBarycentric.pNext = nullptr;
                enabled13.pNext = &enabledBarycentric;
            }
            else
            {
                enabled13.pNext = nullptr;
            }
            enabled12.bufferDeviceAddress = capabilities.bufferDeviceAddress ? VK_TRUE : VK_FALSE;
            deviceExtensions.resize(baseExtensionCount);
            createInfo.enabledExtensionCount = static_cast<std::uint32_t>(baseExtensionCount);
            createInfo.ppEnabledExtensionNames = deviceExtensions.data();
            result = vkCreateDevice(physicalDevice, &createInfo, nullptr, &device);
        }
        if (result != VK_SUCCESS)
        {
            return fail(vkFailure("vkCreateDevice", result));
        }

        vkGetDeviceQueue(device, graphicsQueueFamily, 0, &graphicsQueue);
        vkGetDeviceQueue(device, presentQueueFamily, 0, &presentQueue);
        if (graphicsQueue == VK_NULL_HANDLE || presentQueue == VK_NULL_HANDLE)
        {
            return fail("vkGetDeviceQueue returned a null queue");
        }
        return ok();
    }

    


void VulkanDevice::cleanup() noexcept
{
    if (device != VK_NULL_HANDLE)
    {
        (void)vkDeviceWaitIdle(device);
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }
    if (surface != VK_NULL_HANDLE && instance != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(instance, surface, nullptr);
    }
    surface = VK_NULL_HANDLE;
    if (instance != VK_NULL_HANDLE && debugMessenger != VK_NULL_HANDLE)
    {
        const auto destroyFn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyFn != nullptr)
        {
            destroyFn(instance, debugMessenger, nullptr);
        }
    }
    debugMessenger = VK_NULL_HANDLE;
    if (instance != VK_NULL_HANDLE)
    {
        vkDestroyInstance(instance, nullptr);
    }
    instance = VK_NULL_HANDLE;
    physicalDevice = VK_NULL_HANDLE;
    graphicsQueue = VK_NULL_HANDLE;
    presentQueue = VK_NULL_HANDLE;
    graphicsQueueFamily = VK_QUEUE_FAMILY_IGNORED;
    presentQueueFamily = VK_QUEUE_FAMILY_IGNORED;
    presentTimestampValidBits = 0;
    physicalProperties = {};
    memoryProperties = {};
    capabilities = {};
    deviceLocalBytes = 0;
    rayQueryEnabled = false;
    window_ = nullptr;
    config_ = {};
}
} // namespace Halcyon::Vulkan
