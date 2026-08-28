#include "HalcyonVulkanRenderer.hpp"

// GLFW is included here (rather than in the public header) so applications
// can choose their own GLFW include policy.  The Vulkan include guard makes
// this safe when the caller included glfw3.h with GLFW_INCLUDE_VULKAN first.
#ifndef GLFW_INCLUDE_VULKAN
#    define GLFW_INCLUDE_VULKAN
#endif
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace Halcyon::Vulkan {
namespace {

constexpr std::uint32_t kRequiredApiVersion = VK_API_VERSION_1_3;
constexpr std::uint32_t kDefaultFramesInFlight = 3;
constexpr std::uint32_t kMaxFramesInFlight = 4;

using VoidResult = Halcyon::Result<void>;

[[nodiscard]] VoidResult ok() noexcept {
    return VoidResult::success();
}

[[nodiscard]] VoidResult fail(std::string message,
                              Halcyon::ErrorCode code =
                                  Halcyon::ErrorCode::Backend) {
    return VoidResult::failure(Halcyon::Error{code, std::move(message)});
}

[[nodiscard]] const char* resultName(VkResult result) noexcept {
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_EVENT_SET: return "VK_EVENT_SET";
    case VK_EVENT_RESET: return "VK_EVENT_RESET";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR: return "VK_ERROR_INCOMPATIBLE_DISPLAY_KHR";
    case VK_ERROR_VALIDATION_FAILED_EXT: return "VK_ERROR_VALIDATION_FAILED_EXT";
    default: return "VkResult(unknown)";
    }
}

[[nodiscard]] std::string vkFailure(const char* operation, VkResult result) {
    std::ostringstream stream;
    stream << operation << " failed (" << resultName(result) << ", "
           << static_cast<int>(result) << ')';
    return stream.str();
}

template <typename T>
[[nodiscard]] bool hasName(const std::vector<T>& properties,
                           const char* requested) noexcept {
    for (const auto& property : properties) {
        if (std::strcmp(property.extensionName, requested) == 0) {
            return true;
        }
    }
    return false;
}

template <typename T>
[[nodiscard]] bool hasLayerName(const std::vector<T>& properties,
                                const char* requested) noexcept {
    for (const auto& property : properties) {
        if (std::strcmp(property.layerName, requested) == 0) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::vector<const char*> uniqueNames(
    const std::vector<const char*>& names) {
    std::vector<const char*> result;
    result.reserve(names.size());
    for (const char* name : names) {
        bool duplicate = false;
        for (const char* existing : result) {
            if (std::strcmp(existing, name) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            result.push_back(name);
        }
    }
    return result;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void* /*userData*/) {
    const char* label = "INFO";
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        label = "ERROR";
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
        label = "WARN";
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) != 0) {
        label = "VERBOSE";
    }
    std::fprintf(stderr, "[Halcyon/Vulkan %s] %s\n", label,
                 callbackData != nullptr && callbackData->pMessage != nullptr
                     ? callbackData->pMessage
                     : "<no message>");
    return VK_FALSE;
}

void destroyDebugMessenger(VkInstance instance,
                           VkDebugUtilsMessengerEXT& messenger) noexcept {
    if (instance == VK_NULL_HANDLE || messenger == VK_NULL_HANDLE) {
        messenger = VK_NULL_HANDLE;
        return;
    }
    const auto destroyFn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (destroyFn != nullptr) {
        destroyFn(instance, messenger, nullptr);
    }
    messenger = VK_NULL_HANDLE;
}

struct QueueSelection {
    std::uint32_t graphics = VK_QUEUE_FAMILY_IGNORED;
    std::uint32_t present = VK_QUEUE_FAMILY_IGNORED;

    [[nodiscard]] bool valid() const noexcept {
        return graphics != VK_QUEUE_FAMILY_IGNORED &&
               present != VK_QUEUE_FAMILY_IGNORED;
    }
};

struct DeviceCandidate {
    VkPhysicalDevice handle = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceVulkan12Features features12{};
    VkPhysicalDeviceVulkan13Features features13{};
    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR barycentric{};
    VkPhysicalDeviceRayQueryFeaturesKHR rayQuery{};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructure{};
    QueueSelection queues{};
    bool hasBarycentricExtension = false;
    bool hasRayQueryExtensions = false;
    bool rayQuerySupported = false;
    bool barycentricSupported = false;
    std::uint64_t deviceLocalBytes = 0;
    int score = std::numeric_limits<int>::min();
};

[[nodiscard]] QueueSelection findQueues(VkPhysicalDevice device,
                                         VkSurfaceKHR surface) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    if (count == 0) {
        return {};
    }
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

    QueueSelection selection{};
    // Prefer a family that can do both operations.  A single queue keeps the
    // first slice simple and avoids ownership transfers for the swapchain.
    for (std::uint32_t i = 0; i < count; ++i) {
        if (families[i].queueCount == 0 ||
            (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
            continue;
        }
        VkBool32 present = VK_FALSE;
        if (vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &present) !=
                VK_SUCCESS ||
            present == VK_FALSE) {
            continue;
        }
        selection.graphics = i;
        selection.present = i;
        return selection;
    }

    for (std::uint32_t i = 0; i < count; ++i) {
        if (families[i].queueCount > 0 &&
            (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            selection.graphics = i;
            break;
        }
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        if (families[i].queueCount == 0) {
            continue;
        }
        VkBool32 present = VK_FALSE;
        if (vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &present) ==
                VK_SUCCESS &&
            present != VK_FALSE) {
            selection.present = i;
            break;
        }
    }
    return selection;
}

[[nodiscard]] bool queryDeviceExtensions(VkPhysicalDevice device,
                                         std::vector<VkExtensionProperties>& out) {
    std::uint32_t count = 0;
    VkResult result = vkEnumerateDeviceExtensionProperties(device, nullptr, &count,
                                                            nullptr);
    if (result != VK_SUCCESS) {
        return false;
    }
    out.resize(count);
    if (count != 0) {
        result = vkEnumerateDeviceExtensionProperties(device, nullptr, &count,
                                                       out.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            out.clear();
            return false;
        }
        out.resize(count);
    }
    return true;
}

[[nodiscard]] bool queryCandidateFeatures(DeviceCandidate& candidate,
                                           const std::vector<VkExtensionProperties>&
                                               extensions) {
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

    if (candidate.hasBarycentricExtension) {
        candidate.barycentric = {};
        candidate.barycentric.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR;
        candidate.features13.pNext = &candidate.barycentric;
    }
    if (candidate.hasRayQueryExtensions) {
        candidate.rayQuery = {};
        candidate.rayQuery.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
        candidate.accelerationStructure = {};
        candidate.accelerationStructure.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        if (candidate.hasBarycentricExtension) {
            candidate.barycentric.pNext = &candidate.rayQuery;
        } else {
            candidate.features13.pNext = &candidate.rayQuery;
        }
        candidate.rayQuery.pNext = &candidate.accelerationStructure;
    }

    vkGetPhysicalDeviceFeatures2(candidate.handle, &features2);
    candidate.barycentricSupported = candidate.hasBarycentricExtension &&
                                    candidate.barycentric.fragmentShaderBarycentric;
    candidate.rayQuerySupported = candidate.hasRayQueryExtensions &&
                                  candidate.rayQuery.rayQuery &&
                                  candidate.accelerationStructure.accelerationStructure &&
                                  candidate.features12.bufferDeviceAddress;
    return true;
}

[[nodiscard]] int deviceTypeScore(VkPhysicalDeviceType type) noexcept {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 10000;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 5000;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2500;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return 1000;
    default: return 0;
    }
}

[[nodiscard]] VkSurfaceFormatKHR chooseSurfaceFormat(
    const std::vector<VkSurfaceFormatKHR>& formats) noexcept {
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_R8G8B8A8_SRGB &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    return formats.front();
}

[[nodiscard]] VkPresentModeKHR choosePresentMode(
    const std::vector<VkPresentModeKHR>& modes) noexcept {
    for (VkPresentModeKHR mode : modes) {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
            return mode;
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

[[nodiscard]] VkCompositeAlphaFlagBitsKHR chooseCompositeAlpha(
    VkCompositeAlphaFlagsKHR supported) noexcept {
    constexpr std::array<VkCompositeAlphaFlagBitsKHR, 4> choices = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
    };
    for (auto choice : choices) {
        if ((supported & choice) != 0) {
            return choice;
        }
    }
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

// Select a memory type without pulling a full allocator into the first
// vertical slice.  The helper is deliberately kept local to the Vulkan
// backend; higher layers only deal in resource handles and descriptors.
[[nodiscard]] bool findMemoryType(const VkPhysicalDeviceMemoryProperties& properties,
                                  std::uint32_t typeBits,
                                  VkMemoryPropertyFlags required,
                                  std::uint32_t& typeIndex) noexcept {
    for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        if ((typeBits & (std::uint32_t{1} << index)) != 0 &&
            (properties.memoryTypes[index].propertyFlags & required) == required) {
            typeIndex = index;
            return true;
        }
    }
    return false;
}

// The source of these modules is Shaders/triangle.*.hlsl.  Keeping a tiny
// precompiled copy here makes the vertical slice runnable on machines where
// DXC/glslc is not installed at runtime, while the HLSL remains available for
// shader hot-reload work in later milestones.
constexpr std::array<std::uint32_t, 273> kTriangleVertexSpirv = {
    0x07230203u, 0x00010600u, 0x000E0000u, 0x00000031u, 0x00000000u, 0x00020011u, 0x00000001u, 0x0003000Eu,
    0x00000000u, 0x00000001u, 0x0008000Fu, 0x00000000u, 0x00000001u, 0x6E69616Du, 0x00000000u, 0x00000002u,
    0x00000003u, 0x00000004u, 0x00030003u, 0x00000005u, 0x00000258u, 0x00060005u, 0x00000004u, 0x2E74756Fu,
    0x2E726176u, 0x4F4C4F43u, 0x00003052u, 0x00050005u, 0x00000005u, 0x69736F70u, 0x6E6F6974u, 0x00000073u,
    0x00040005u, 0x00000006u, 0x6F6C6F63u, 0x00007372u, 0x00040005u, 0x00000001u, 0x6E69616Du, 0x00000000u,
    0x00040047u, 0x00000002u, 0x0000000Bu, 0x0000002Au, 0x00040047u, 0x00000003u, 0x0000000Bu, 0x00000000u,
    0x00040047u, 0x00000004u, 0x0000001Eu, 0x00000000u, 0x00030016u, 0x00000007u, 0x00000020u, 0x0004002Bu,
    0x00000007u, 0x00000008u, 0x00000000u, 0x0004002Bu, 0x00000007u, 0x00000009u, 0xBF3851ECu, 0x00040017u,
    0x0000000Au, 0x00000007u, 0x00000002u, 0x0005002Cu, 0x0000000Au, 0x0000000Bu, 0x00000008u, 0x00000009u,
    0x0004002Bu, 0x00000007u, 0x0000000Cu, 0x3F3851ECu, 0x0005002Cu, 0x0000000Au, 0x0000000Du, 0x0000000Cu,
    0x0000000Cu, 0x0005002Cu, 0x0000000Au, 0x0000000Eu, 0x00000009u, 0x0000000Cu, 0x0004002Bu, 0x00000007u,
    0x0000000Fu, 0x3F800000u, 0x0004002Bu, 0x00000007u, 0x00000010u, 0x3E800000u, 0x0004002Bu, 0x00000007u,
    0x00000011u, 0x3E4CCCCDu, 0x00040017u, 0x00000012u, 0x00000007u, 0x00000003u, 0x0006002Cu, 0x00000012u,
    0x00000013u, 0x0000000Fu, 0x00000010u, 0x00000011u, 0x0004002Bu, 0x00000007u, 0x00000014u, 0x3EB33333u,
    0x0006002Cu, 0x00000012u, 0x00000015u, 0x00000011u, 0x0000000Fu, 0x00000014u, 0x0004002Bu, 0x00000007u,
    0x00000016u, 0x3EE66666u, 0x0006002Cu, 0x00000012u, 0x00000017u, 0x00000011u, 0x00000016u, 0x0000000Fu,
    0x00040015u, 0x00000018u, 0x00000020u, 0x00000000u, 0x00040020u, 0x00000019u, 0x00000001u, 0x00000018u,
    0x00040017u, 0x0000001Au, 0x00000007u, 0x00000004u, 0x00040020u, 0x0000001Bu, 0x00000003u, 0x0000001Au,
    0x00040020u, 0x0000001Cu, 0x00000003u, 0x00000012u, 0x0004002Bu, 0x00000018u, 0x0000001Du, 0x00000003u,
    0x0004001Cu, 0x0000001Eu, 0x0000000Au, 0x0000001Du, 0x0004001Cu, 0x0000001Fu, 0x00000012u, 0x0000001Du,
    0x00020013u, 0x00000020u, 0x00030021u, 0x00000021u, 0x00000020u, 0x00040020u, 0x00000022u, 0x00000007u,
    0x00000012u, 0x0004003Bu, 0x00000019u, 0x00000002u, 0x00000001u, 0x0004003Bu, 0x0000001Bu, 0x00000003u,
    0x00000003u, 0x0004003Bu, 0x0000001Cu, 0x00000004u, 0x00000003u, 0x00040020u, 0x00000023u, 0x00000007u,
    0x0000001Eu, 0x00040020u, 0x00000024u, 0x00000007u, 0x0000000Au, 0x00040020u, 0x00000025u, 0x00000007u,
    0x0000001Fu, 0x0006002Cu, 0x0000001Eu, 0x00000026u, 0x0000000Bu, 0x0000000Du, 0x0000000Eu, 0x0006002Cu,
    0x0000001Fu, 0x00000027u, 0x00000013u, 0x00000015u, 0x00000017u, 0x00050036u, 0x00000020u, 0x00000001u,
    0x00000000u, 0x00000021u, 0x000200F8u, 0x00000028u, 0x0004003Bu, 0x00000025u, 0x00000006u, 0x00000007u,
    0x0004003Bu, 0x00000023u, 0x00000005u, 0x00000007u, 0x0004003Du, 0x00000018u, 0x00000029u, 0x00000002u,
    0x0003003Eu, 0x00000005u, 0x00000026u, 0x0003003Eu, 0x00000006u, 0x00000027u, 0x00050041u, 0x00000024u,
    0x0000002Au, 0x00000005u, 0x00000029u, 0x0004003Du, 0x0000000Au, 0x0000002Bu, 0x0000002Au, 0x00050051u,
    0x00000007u, 0x0000002Cu, 0x0000002Bu, 0x00000000u, 0x00050051u, 0x00000007u, 0x0000002Du, 0x0000002Bu,
    0x00000001u, 0x00070050u, 0x0000001Au, 0x0000002Eu, 0x0000002Cu, 0x0000002Du, 0x00000008u, 0x0000000Fu,
    0x00050041u, 0x00000022u, 0x0000002Fu, 0x00000006u, 0x00000029u, 0x0004003Du, 0x00000012u, 0x00000030u,
    0x0000002Fu, 0x0003003Eu, 0x00000003u, 0x0000002Eu, 0x0003003Eu, 0x00000004u, 0x00000030u, 0x000100FDu,
    0x00010038u,
};

constexpr std::array<std::uint32_t, 122> kTriangleFragmentSpirv = {
    0x07230203u, 0x00010600u, 0x000E0000u, 0x00000012u, 0x00000000u, 0x00020011u, 0x00000001u, 0x0003000Eu,
    0x00000000u, 0x00000001u, 0x0007000Fu, 0x00000004u, 0x00000001u, 0x6E69616Du, 0x00000000u, 0x00000002u,
    0x00000003u, 0x00030010u, 0x00000001u, 0x00000007u, 0x00030003u, 0x00000005u, 0x00000258u, 0x00060005u,
    0x00000002u, 0x762E6E69u, 0x432E7261u, 0x524F4C4Fu, 0x00000030u, 0x00070005u, 0x00000003u, 0x2E74756Fu,
    0x2E726176u, 0x545F5653u, 0x65677261u, 0x00003074u, 0x00040005u, 0x00000001u, 0x6E69616Du, 0x00000000u,
    0x00040047u, 0x00000002u, 0x0000001Eu, 0x00000000u, 0x00040047u, 0x00000003u, 0x0000001Eu, 0x00000000u,
    0x00030016u, 0x00000004u, 0x00000020u, 0x0004002Bu, 0x00000004u, 0x00000005u, 0x3F800000u, 0x00040017u,
    0x00000006u, 0x00000004u, 0x00000004u, 0x00040017u, 0x00000007u, 0x00000004u, 0x00000003u, 0x00040020u,
    0x00000008u, 0x00000001u, 0x00000007u, 0x00040020u, 0x00000009u, 0x00000003u, 0x00000006u, 0x00020013u,
    0x0000000Au, 0x00030021u, 0x0000000Bu, 0x0000000Au, 0x0004003Bu, 0x00000008u, 0x00000002u, 0x00000001u,
    0x0004003Bu, 0x00000009u, 0x00000003u, 0x00000003u, 0x00050036u, 0x0000000Au, 0x00000001u, 0x00000000u,
    0x0000000Bu, 0x000200F8u, 0x0000000Cu, 0x0004003Du, 0x00000007u, 0x0000000Du, 0x00000002u, 0x00050051u,
    0x00000004u, 0x0000000Eu, 0x0000000Du, 0x00000000u, 0x00050051u, 0x00000004u, 0x0000000Fu, 0x0000000Du,
    0x00000001u, 0x00050051u, 0x00000004u, 0x00000010u, 0x0000000Du, 0x00000002u, 0x00070050u, 0x00000006u,
    0x00000011u, 0x0000000Eu, 0x0000000Fu, 0x00000010u, 0x00000005u, 0x0003003Eu, 0x00000003u, 0x00000011u,
    0x000100FDu, 0x00010038u,
};

} // namespace

struct Renderer::Impl {
    struct FrameContext {
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        std::uint64_t timelineValue = 0;
        std::uint32_t queryBase = 0;
        bool submitted = false;
    };

    RendererConfig config{};
    GLFWwindow* window = nullptr;
    Capabilities caps{};
    std::string lastError;

    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties physicalProperties{};
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;
    std::uint32_t graphicsQueueFamily = VK_QUEUE_FAMILY_IGNORED;
    std::uint32_t presentQueueFamily = VK_QUEUE_FAMILY_IGNORED;
    std::uint32_t presentTimestampValidBits = 0;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR swapchainColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D swapchainExtent{};
    // The depth target is recreated together with the swapchain.  D32 is
    // intentional: it gives reversed-Z the precision it needs and keeps the
    // image usable by later Hi-Z experiments without a stencil dependency.
    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    VkImageView depthImageView = VK_NULL_HANDLE;
    VkDeviceSize depthMemorySize = 0;
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    bool depthImageInitialized = false;
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainImageViews;
    // Presentation waits are indexed by acquired image, not by frame.  Once
    // an image is acquired again its previous presentation wait is known to
    // have retired, so its binary semaphore can safely be reused.
    std::vector<VkSemaphore> presentReadySemaphores;
    std::vector<bool> swapchainImageInitialized;

    std::vector<FrameContext> frames;
    std::uint32_t currentFrame = 0;
    VkSemaphore timelineSemaphore = VK_NULL_HANDLE;
    std::uint64_t nextTimelineValue = 1;

    VkQueryPool timestampPool = VK_NULL_HANDLE;
    bool timestampsEnabled = false;
    float timestampPeriod = 1.0f;

    VkPipelineLayout trianglePipelineLayout = VK_NULL_HANDLE;
    VkPipeline trianglePipeline = VK_NULL_HANDLE;
    VkShaderModule triangleVertexShader = VK_NULL_HANDLE;
    VkShaderModule triangleFragmentShader = VK_NULL_HANDLE;

    Extent2D requestedExtent{};
    bool framebufferResized = false;
    bool initialized = false;
    bool deviceLost = false;
    bool fatalError = false;
    bool rayQueryEnabled = false;
    std::uint64_t deviceLocalBytes = 0;
    VkDeviceSize deviceMemoryBytes = 0;

    ~Impl() { cleanup(); }

    void setError(std::string message) { lastError = std::move(message); }

    void destroyDepthResources() noexcept {
        if (device != VK_NULL_HANDLE) {
            if (depthImageView != VK_NULL_HANDLE) {
                vkDestroyImageView(device, depthImageView, nullptr);
            }
            if (depthImage != VK_NULL_HANDLE) {
                vkDestroyImage(device, depthImage, nullptr);
            }
            if (depthMemory != VK_NULL_HANDLE) {
                vkFreeMemory(device, depthMemory, nullptr);
            }
        }
        depthImageView = VK_NULL_HANDLE;
        depthImage = VK_NULL_HANDLE;
        depthMemory = VK_NULL_HANDLE;
        depthMemorySize = 0;
        depthImageInitialized = false;
        deviceMemoryBytes = 0;
    }

    void cleanupSwapchain() noexcept {
        if (device != VK_NULL_HANDLE) {
            destroyTrianglePipeline();
            destroyDepthResources();
            for (VkImageView view : swapchainImageViews) {
                if (view != VK_NULL_HANDLE) {
                    vkDestroyImageView(device, view, nullptr);
                }
            }
            for (VkSemaphore semaphore : presentReadySemaphores) {
                if (semaphore != VK_NULL_HANDLE) {
                    vkDestroySemaphore(device, semaphore, nullptr);
                }
            }
        }
        swapchainImageViews.clear();
        presentReadySemaphores.clear();
        swapchainImages.clear();
        swapchainImageInitialized.clear();
        if (device != VK_NULL_HANDLE && swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device, swapchain, nullptr);
        }
        swapchain = VK_NULL_HANDLE;
        swapchainFormat = VK_FORMAT_UNDEFINED;
        swapchainColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        swapchainExtent = {};
    }

    void cleanup() noexcept {
        if (device != VK_NULL_HANDLE) {
            // Waiting is best effort during error cleanup; every child object
            // is still destroyed even when the device has already been lost.
            (void)vkDeviceWaitIdle(device);
            cleanupSwapchain();
            if (timestampPool != VK_NULL_HANDLE) {
                vkDestroyQueryPool(device, timestampPool, nullptr);
                timestampPool = VK_NULL_HANDLE;
            }
            for (auto& frame : frames) {
                if (frame.fence != VK_NULL_HANDLE) {
                    vkDestroyFence(device, frame.fence, nullptr);
                }
                if (frame.imageAvailable != VK_NULL_HANDLE) {
                    vkDestroySemaphore(device, frame.imageAvailable, nullptr);
                }
                if (frame.commandPool != VK_NULL_HANDLE) {
                    vkDestroyCommandPool(device, frame.commandPool, nullptr);
                }
            }
            frames.clear();
            if (timelineSemaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(device, timelineSemaphore, nullptr);
                timelineSemaphore = VK_NULL_HANDLE;
            }
            vkDestroyDevice(device, nullptr);
            device = VK_NULL_HANDLE;
        }
        if (surface != VK_NULL_HANDLE && instance != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance, surface, nullptr);
        }
        surface = VK_NULL_HANDLE;
        destroyDebugMessenger(instance, debugMessenger);
        if (instance != VK_NULL_HANDLE) {
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
        initialized = false;
        deviceLost = false;
        fatalError = false;
        rayQueryEnabled = false;
        depthFormat = VK_FORMAT_D32_SFLOAT;
        deviceMemoryBytes = 0;
        timestampsEnabled = false;
        timestampPeriod = 1.0f;
        currentFrame = 0;
        nextTimelineValue = 1;
        framebufferResized = false;
        requestedExtent = {};
        window = nullptr;
        caps = {};
        deviceLocalBytes = 0;
    }

    [[nodiscard]] VoidResult createInstance() {
        std::uint32_t loaderVersion = VK_API_VERSION_1_0;
        // vkEnumerateInstanceVersion was introduced in Vulkan 1.1.  The
        // loader linked by the Vulkan SDK always exposes it, but accepting a
        // graceful error here makes startup diagnostics clearer on old hosts.
        VkResult result = vkEnumerateInstanceVersion(&loaderVersion);
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            return fail(vkFailure("vkEnumerateInstanceVersion", result));
        }
        caps.instanceApiVersion = loaderVersion;
        if (VK_VERSION_MAJOR(loaderVersion) < 1 ||
            (VK_VERSION_MAJOR(loaderVersion) == 1 &&
             VK_VERSION_MINOR(loaderVersion) < 3)) {
            return fail(
                "A Vulkan 1.3 loader is required by the Halcyon M1 renderer");
        }

        std::uint32_t extensionCount = 0;
        result = vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount,
                                                         nullptr);
        if (result != VK_SUCCESS) {
            return fail(
                vkFailure("vkEnumerateInstanceExtensionProperties", result));
        }
        std::vector<VkExtensionProperties> availableExtensions(extensionCount);
        if (extensionCount != 0) {
            result = vkEnumerateInstanceExtensionProperties(
                nullptr, &extensionCount, availableExtensions.data());
            if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
                return fail(
                    vkFailure("vkEnumerateInstanceExtensionProperties", result));
            }
            availableExtensions.resize(extensionCount);
        }

        std::uint32_t layerCount = 0;
        result = vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        if (result != VK_SUCCESS) {
            return fail(
                vkFailure("vkEnumerateInstanceLayerProperties", result));
        }
        std::vector<VkLayerProperties> availableLayers(layerCount);
        if (layerCount != 0) {
            result = vkEnumerateInstanceLayerProperties(&layerCount,
                                                        availableLayers.data());
            if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
                return fail(
                    vkFailure("vkEnumerateInstanceLayerProperties", result));
            }
            availableLayers.resize(layerCount);
        }

        std::uint32_t glfwExtensionCount = 0;
        const char* const* glfwExtensions =
            glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
        if (glfwExtensions == nullptr || glfwExtensionCount == 0) {
            return fail(
                "GLFW did not provide the required Vulkan instance extensions");
        }
        std::vector<const char*> extensions(glfwExtensions,
                                            glfwExtensions + glfwExtensionCount);

        const bool debugUtilsAvailable =
            hasName(availableExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        if (config.enableValidation && debugUtilsAvailable) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            caps.debugUtils = true;
        }
        extensions = uniqueNames(extensions);
        for (const char* extension : extensions) {
            if (!hasName(availableExtensions, extension)) {
                return fail(std::string("Required instance extension is not "
                                                   "available: ") +
                                       extension);
            }
        }

        std::vector<const char*> layers;
        const bool validationAvailable =
            hasLayerName(availableLayers, "VK_LAYER_KHRONOS_validation");
        if (config.enableValidation && validationAvailable) {
            layers.push_back("VK_LAYER_KHRONOS_validation");
            caps.validationEnabled = true;
        }

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = config.applicationName != nullptr
                                       ? config.applicationName
                                       : "Halcyon";
        appInfo.applicationVersion = config.applicationVersion;
        appInfo.pEngineName = "Halcyon Engine";
        appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        appInfo.apiVersion = kRequiredApiVersion;

        VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
        debugCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        debugCreateInfo.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debugCreateInfo.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debugCreateInfo.pfnUserCallback = debugCallback;

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount =
            static_cast<std::uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
        createInfo.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
        createInfo.ppEnabledLayerNames = layers.data();
        if (caps.debugUtils) {
            createInfo.pNext = &debugCreateInfo;
        }

        result = vkCreateInstance(&createInfo, nullptr, &instance);
        if (result != VK_SUCCESS) {
            return fail(vkFailure("vkCreateInstance", result));
        }

        if (caps.debugUtils) {
            const auto createDebugFn =
                reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                    vkGetInstanceProcAddr(instance,
                                          "vkCreateDebugUtilsMessengerEXT"));
            if (createDebugFn != nullptr) {
                result = createDebugFn(instance, &debugCreateInfo, nullptr,
                                       &debugMessenger);
                if (result != VK_SUCCESS) {
                    // Debug output is useful but must never make a renderer
                    // unusable when a loader advertises a partial extension.
                    debugMessenger = VK_NULL_HANDLE;
                    caps.debugUtils = false;
                }
            } else {
                caps.debugUtils = false;
            }
        }
        return ok();
    }

    [[nodiscard]] VoidResult createSurface() {
        if (window == nullptr) {
            return fail("Renderer::initialize received a null GLFWwindow");
        }
        const VkResult result =
            glfwCreateWindowSurface(instance, window, nullptr, &surface);
        if (result != VK_SUCCESS) {
            return fail(vkFailure("glfwCreateWindowSurface", result));
        }
        return ok();
    }

    [[nodiscard]] VoidResult pickPhysicalDevice() {
        std::uint32_t count = 0;
        VkResult result = vkEnumeratePhysicalDevices(instance, &count, nullptr);
        if (result != VK_SUCCESS) {
            return fail(vkFailure("vkEnumeratePhysicalDevices", result));
        }
        if (count == 0) {
            return fail("No Vulkan physical device was found");
        }
        std::vector<VkPhysicalDevice> devices(count);
        result = vkEnumeratePhysicalDevices(instance, &count, devices.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            return fail(vkFailure("vkEnumeratePhysicalDevices", result));
        }
        devices.resize(count);

        DeviceCandidate best{};
        for (VkPhysicalDevice candidateHandle : devices) {
            DeviceCandidate candidate{};
            candidate.handle = candidateHandle;
            vkGetPhysicalDeviceProperties(candidate.handle, &candidate.properties);
            if (candidate.properties.apiVersion < kRequiredApiVersion) {
                continue;
            }

            std::vector<VkExtensionProperties> extensions;
            if (!queryDeviceExtensions(candidate.handle, extensions) ||
                !hasName(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
                continue;
            }
            candidate.queues = findQueues(candidate.handle, surface);
            if (!candidate.queues.valid()) {
                continue;
            }
            if (!queryCandidateFeatures(candidate, extensions)) {
                continue;
            }
            if (config.rayQuery == FeatureMode::Required &&
                !candidate.rayQuerySupported) {
                continue;
            }
            if (candidate.features13.dynamicRendering == VK_FALSE ||
                candidate.features13.synchronization2 == VK_FALSE ||
                candidate.features12.timelineSemaphore == VK_FALSE) {
                continue;
            }

            VkFormatProperties depthProperties{};
            vkGetPhysicalDeviceFormatProperties(candidate.handle,
                                                 VK_FORMAT_D32_SFLOAT,
                                                 &depthProperties);
            if ((depthProperties.optimalTilingFeatures &
                 VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0) {
                // M1 intentionally has one well-defined depth format.  A
                // later capability tier can add D24/D32S8 fallbacks without
                // silently changing reversed-Z precision here.
                continue;
            }

            std::uint32_t surfaceFormatCount = 0;
            std::uint32_t presentModeCount = 0;
            if (vkGetPhysicalDeviceSurfaceFormatsKHR(candidate.handle, surface,
                                                     &surfaceFormatCount, nullptr) !=
                    VK_SUCCESS ||
                vkGetPhysicalDeviceSurfacePresentModesKHR(
                    candidate.handle, surface, &presentModeCount, nullptr) !=
                    VK_SUCCESS ||
                surfaceFormatCount == 0 || presentModeCount == 0) {
                continue;
            }

            VkPhysicalDeviceMemoryProperties memory{};
            vkGetPhysicalDeviceMemoryProperties(candidate.handle, &memory);
            for (std::uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
                if ((memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) !=
                    0) {
                    candidate.deviceLocalBytes += memory.memoryHeaps[i].size;
                }
            }
            candidate.score = deviceTypeScore(candidate.properties.deviceType) +
                              static_cast<int>(candidate.properties.limits.maxImageDimension2D);
            if (candidate.score > best.score) {
                best = candidate;
            }
        }

        if (best.handle == VK_NULL_HANDLE) {
            return fail(
                "No Vulkan 1.3 device with graphics/present queues, swapchain, "
                "dynamic rendering, synchronization2 and timeline semaphores was found");
        }

        physicalDevice = best.handle;
        physicalProperties = best.properties;
        graphicsQueueFamily = best.queues.graphics;
        presentQueueFamily = best.queues.present;
        deviceLocalBytes = best.deviceLocalBytes;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);

        caps.deviceApiVersion = physicalProperties.apiVersion;
        caps.deviceName = physicalProperties.deviceName;
        caps.vendorId = physicalProperties.vendorID;
        caps.deviceId = physicalProperties.deviceID;
        caps.deviceLocalMemoryBytes = best.deviceLocalBytes;
        caps.dynamicRendering = best.features13.dynamicRendering != VK_FALSE;
        caps.synchronization2 = best.features13.synchronization2 != VK_FALSE;
        caps.timelineSemaphore = best.features12.timelineSemaphore != VK_FALSE;
        caps.descriptorIndexing = best.features12.descriptorIndexing != VK_FALSE;
        caps.bufferDeviceAddress = best.features12.bufferDeviceAddress != VK_FALSE;
        caps.indirectCount = best.features12.drawIndirectCount != VK_FALSE;
        caps.fragmentBarycentric = best.barycentricSupported;
        caps.rayQuery = best.rayQuerySupported;
        caps.depthD32 = true;
        caps.reversedZ = true;
        caps.swapchain = true;
        caps.graphicsQueueFamily = graphicsQueueFamily;
        caps.presentQueueFamily = presentQueueFamily;

        if (config.rayQuery == FeatureMode::Required && !caps.rayQuery) {
            return fail(
                "Ray Query was required but the selected Vulkan device does not support it");
        }
        return ok();
    }

    [[nodiscard]] VoidResult createDevice() {
        std::set<std::uint32_t> uniqueQueueFamilies = {graphicsQueueFamily,
                                                        presentQueueFamily};
        const float queuePriority = 1.0f;
        std::vector<VkDeviceQueueCreateInfo> queueInfos;
        queueInfos.reserve(uniqueQueueFamilies.size());
        for (std::uint32_t family : uniqueQueueFamilies) {
            VkDeviceQueueCreateInfo queueInfo{};
            queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueInfo.queueFamilyIndex = family;
            queueInfo.queueCount = 1;
            queueInfo.pQueuePriorities = &queuePriority;
            queueInfos.push_back(queueInfo);
        }

        std::uint32_t extensionCount = 0;
        VkResult result = vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr,
                                                                &extensionCount, nullptr);
        if (result != VK_SUCCESS) {
            return fail(
                vkFailure("vkEnumerateDeviceExtensionProperties", result));
        }
        std::vector<VkExtensionProperties> availableExtensions(extensionCount);
        if (extensionCount != 0) {
            result = vkEnumerateDeviceExtensionProperties(
                physicalDevice, nullptr, &extensionCount, availableExtensions.data());
            if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
                return fail(
                    vkFailure("vkEnumerateDeviceExtensionProperties", result));
            }
            availableExtensions.resize(extensionCount);
        }

        std::vector<const char*> deviceExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        if (caps.fragmentBarycentric) {
            deviceExtensions.push_back(
                VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);
        }
        const std::size_t baseExtensionCount = deviceExtensions.size();
        const bool canUseRayQuery = caps.rayQuery &&
                                    config.rayQuery != FeatureMode::Disabled;
        if (canUseRayQuery) {
            deviceExtensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
            deviceExtensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
            deviceExtensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        }
        for (const char* extension : deviceExtensions) {
            if (!hasName(availableExtensions, extension)) {
                if (canUseRayQuery && config.rayQuery == FeatureMode::Auto) {
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
        enabled12.descriptorIndexing = caps.descriptorIndexing ? VK_TRUE : VK_FALSE;
        enabled12.bufferDeviceAddress = caps.bufferDeviceAddress ? VK_TRUE : VK_FALSE;
        enabled12.drawIndirectCount = caps.indirectCount ? VK_TRUE : VK_FALSE;
        VkPhysicalDeviceVulkan13Features enabled13{};
        enabled13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        enabled13.dynamicRendering = VK_TRUE;
        enabled13.synchronization2 = VK_TRUE;
        enabledFeatures.pNext = &enabled12;
        enabled12.pNext = &enabled13;

        VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR enabledBarycentric{};
        if (caps.fragmentBarycentric) {
            enabledBarycentric.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR;
            enabledBarycentric.fragmentShaderBarycentric = VK_TRUE;
            enabled13.pNext = &enabledBarycentric;
        }

        VkPhysicalDeviceRayQueryFeaturesKHR enabledRay{};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR enabledAcceleration{};
        if (canUseRayQuery &&
            deviceExtensions.size() >= baseExtensionCount + 3u) {
            enabledRay.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
            enabledRay.rayQuery = VK_TRUE;
            enabledAcceleration.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
            enabledAcceleration.accelerationStructure = VK_TRUE;
            if (caps.fragmentBarycentric) {
                enabledBarycentric.pNext = &enabledRay;
            } else {
                enabled13.pNext = &enabledRay;
            }
            enabledRay.pNext = &enabledAcceleration;
            rayQueryEnabled = true;
        }

        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.pNext = &enabledFeatures;
        createInfo.queueCreateInfoCount =
            static_cast<std::uint32_t>(queueInfos.size());
        createInfo.pQueueCreateInfos = queueInfos.data();
        createInfo.enabledExtensionCount =
            static_cast<std::uint32_t>(deviceExtensions.size());
        createInfo.ppEnabledExtensionNames = deviceExtensions.data();

        result = vkCreateDevice(physicalDevice, &createInfo, nullptr, &device);
        if (result != VK_SUCCESS && rayQueryEnabled &&
            config.rayQuery == FeatureMode::Auto) {
            // Retry the Base feature set if an optional RT extension fails
            // device creation.  This is intentionally only done for Auto;
            // Required surfaces the original error to the caller.
            device = VK_NULL_HANDLE;
            rayQueryEnabled = false;
            if (caps.fragmentBarycentric) {
                enabledBarycentric.pNext = nullptr;
                enabled13.pNext = &enabledBarycentric;
            } else {
                enabled13.pNext = nullptr;
            }
            enabled12.bufferDeviceAddress =
                caps.bufferDeviceAddress ? VK_TRUE : VK_FALSE;
            deviceExtensions.resize(baseExtensionCount);
            createInfo.enabledExtensionCount =
                static_cast<std::uint32_t>(baseExtensionCount);
            createInfo.ppEnabledExtensionNames = deviceExtensions.data();
            result = vkCreateDevice(physicalDevice, &createInfo, nullptr, &device);
        }
        if (result != VK_SUCCESS) {
            return fail(vkFailure("vkCreateDevice", result));
        }

        vkGetDeviceQueue(device, graphicsQueueFamily, 0, &graphicsQueue);
        vkGetDeviceQueue(device, presentQueueFamily, 0, &presentQueue);
        if (graphicsQueue == VK_NULL_HANDLE || presentQueue == VK_NULL_HANDLE) {
            return fail("vkGetDeviceQueue returned a null queue");
        }
        return ok();
    }

    [[nodiscard]] VoidResult createTimelineSemaphore() {
        VkSemaphoreTypeCreateInfo typeInfo{};
        typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        typeInfo.initialValue = 0;

        VkSemaphoreCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        createInfo.pNext = &typeInfo;
        const VkResult result =
            vkCreateSemaphore(device, &createInfo, nullptr, &timelineSemaphore);
        if (result != VK_SUCCESS) {
            return fail(vkFailure("vkCreateSemaphore(timeline)", result));
        }
        return ok();
    }

    [[nodiscard]] VoidResult createFrameResources() {
        const std::uint32_t frameCount = std::clamp(
            config.framesInFlight == 0 ? kDefaultFramesInFlight : config.framesInFlight,
            2u, kMaxFramesInFlight);
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

        for (std::uint32_t i = 0; i < frameCount; ++i) {
            auto& frame = frames[i];
            VkResult result = vkCreateCommandPool(device, &poolInfo, nullptr,
                                                   &frame.commandPool);
            if (result != VK_SUCCESS) {
                return fail(vkFailure("vkCreateCommandPool", result));
            }
            allocateInfo.commandPool = frame.commandPool;
            result = vkAllocateCommandBuffers(device, &allocateInfo,
                                               &frame.commandBuffer);
            if (result != VK_SUCCESS) {
                return fail(
                    vkFailure("vkAllocateCommandBuffers", result));
            }
            result = vkCreateSemaphore(device, &semaphoreInfo, nullptr,
                                       &frame.imageAvailable);
            if (result != VK_SUCCESS) {
                return fail(
                    vkFailure("vkCreateSemaphore(imageAvailable)", result));
            }
            result = vkCreateFence(device, &fenceInfo, nullptr, &frame.fence);
            if (result != VK_SUCCESS) {
                return fail(vkFailure("vkCreateFence", result));
            }
            frame.queryBase = i * 2;
        }

        // Timestamps are optional: a queue with timestampValidBits == 0 is
        // legal, so rendering remains functional without this instrumentation.
        std::uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount,
                                                  nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount,
                                                 queueFamilies.data());
        if (graphicsQueueFamily < queueFamilies.size()) {
            presentTimestampValidBits =
                queueFamilies[graphicsQueueFamily].timestampValidBits;
        }
        timestampPeriod = physicalProperties.limits.timestampPeriod;
        if (presentTimestampValidBits != 0 && timestampPeriod > 0.0f) {
            VkQueryPoolCreateInfo queryInfo{};
            queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queryInfo.queryCount = frameCount * 2;
            const VkResult result =
                vkCreateQueryPool(device, &queryInfo, nullptr, &timestampPool);
            if (result == VK_SUCCESS) {
                timestampsEnabled = true;
            }
        }
        return ok();
    }

    // Create a depth attachment for one swapchain extent.  Handles are
    // returned separately so createSwapchain() can commit them transactionally
    // and leave the old swapchain/depth target intact if allocation fails.
    [[nodiscard]] VoidResult createDepthResources(VkExtent2D extent,
                                                   VkImage& outImage,
                                                   VkDeviceMemory& outMemory,
                                                   VkImageView& outView,
                                                   VkDeviceSize& outMemorySize) {
        outImage = VK_NULL_HANDLE;
        outMemory = VK_NULL_HANDLE;
        outView = VK_NULL_HANDLE;
        outMemorySize = 0;
        if (extent.width == 0 || extent.height == 0) {
            return fail("Cannot create a depth image with a zero extent",
                        Halcyon::ErrorCode::InvalidArgument);
        }

        VkFormatProperties formatProperties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice, depthFormat,
                                             &formatProperties);
        if ((formatProperties.optimalTilingFeatures &
             VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0) {
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

        VkResult result = vkCreateImage(device, &imageInfo, nullptr, &outImage);
        if (result != VK_SUCCESS) {
            outImage = VK_NULL_HANDLE;
            return fail(vkFailure("vkCreateImage(depth)", result));
        }

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, outImage, &requirements);
        std::uint32_t memoryType = 0;
        if (!findMemoryType(memoryProperties, requirements.memoryTypeBits,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memoryType)) {
            vkDestroyImage(device, outImage, nullptr);
            outImage = VK_NULL_HANDLE;
            return fail("No device-local memory type is available for the depth image",
                        Halcyon::ErrorCode::OutOfMemory);
        }

        VkMemoryAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex = memoryType;
        result = vkAllocateMemory(device, &allocateInfo, nullptr, &outMemory);
        if (result != VK_SUCCESS) {
            vkDestroyImage(device, outImage, nullptr);
            outImage = VK_NULL_HANDLE;
            outMemory = VK_NULL_HANDLE;
            return fail(vkFailure("vkAllocateMemory(depth)", result));
        }

        result = vkBindImageMemory(device, outImage, outMemory, 0);
        if (result != VK_SUCCESS) {
            vkFreeMemory(device, outMemory, nullptr);
            vkDestroyImage(device, outImage, nullptr);
            outMemory = VK_NULL_HANDLE;
            outImage = VK_NULL_HANDLE;
            return fail(vkFailure("vkBindImageMemory(depth)", result));
        }

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
        result = vkCreateImageView(device, &viewInfo, nullptr, &outView);
        if (result != VK_SUCCESS) {
            vkFreeMemory(device, outMemory, nullptr);
            vkDestroyImage(device, outImage, nullptr);
            outView = VK_NULL_HANDLE;
            outMemory = VK_NULL_HANDLE;
            outImage = VK_NULL_HANDLE;
            return fail(vkFailure("vkCreateImageView(depth)", result));
        }
        outMemorySize = requirements.size;
        return ok();
    }

    [[nodiscard]] VoidResult createSwapchain() {
        VkSurfaceCapabilitiesKHR capabilities{};
        VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            physicalDevice, surface, &capabilities);
        if (result != VK_SUCCESS) {
            return fail(
                vkFailure("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", result));
        }
        if ((capabilities.supportedUsageFlags &
             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0) {
            return fail("The surface does not support color-attachment swapchain images",
                        Halcyon::ErrorCode::Unsupported);
        }

        std::uint32_t formatCount = 0;
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface,
                                                      &formatCount, nullptr);
        if (result != VK_SUCCESS || formatCount == 0) {
            return fail(
                result != VK_SUCCESS
                    ? vkFailure("vkGetPhysicalDeviceSurfaceFormatsKHR", result)
                    : "The surface exposes no formats");
        }
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface,
                                                      &formatCount, formats.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            return fail(
                vkFailure("vkGetPhysicalDeviceSurfaceFormatsKHR", result));
        }
        formats.resize(formatCount);

        std::uint32_t presentModeCount = 0;
        result = vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface,
                                                           &presentModeCount, nullptr);
        if (result != VK_SUCCESS || presentModeCount == 0) {
            return fail(
                result != VK_SUCCESS
                    ? vkFailure("vkGetPhysicalDeviceSurfacePresentModesKHR", result)
                    : "The surface exposes no present modes");
        }
        std::vector<VkPresentModeKHR> presentModes(presentModeCount);
        result = vkGetPhysicalDeviceSurfacePresentModesKHR(
            physicalDevice, surface, &presentModeCount, presentModes.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            return fail(
                vkFailure("vkGetPhysicalDeviceSurfacePresentModesKHR", result));
        }
        presentModes.resize(presentModeCount);

        VkExtent2D extent{};
        if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
            extent = capabilities.currentExtent;
        } else {
            int framebufferWidth = 0;
            int framebufferHeight = 0;
            glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
            const std::uint32_t requestedWidth = framebufferWidth > 0
                                                     ? static_cast<std::uint32_t>(
                                                           framebufferWidth)
                                                     : requestedExtent.width;
            const std::uint32_t requestedHeight = framebufferHeight > 0
                                                      ? static_cast<std::uint32_t>(
                                                            framebufferHeight)
                                                      : requestedExtent.height;
            extent.width = std::clamp(requestedWidth, capabilities.minImageExtent.width,
                                      capabilities.maxImageExtent.width);
            extent.height = std::clamp(requestedHeight, capabilities.minImageExtent.height,
                                       capabilities.maxImageExtent.height);
        }
        if (extent.width == 0 || extent.height == 0) {
            // Minimized windows have a zero framebuffer.  Leave the existing
            // swapchain intact and let render() retry after restoration.
            return ok();
        }

        const VkSurfaceFormatKHR surfaceFormat = chooseSurfaceFormat(formats);
        const VkPresentModeKHR presentMode = choosePresentMode(presentModes);
        std::uint32_t imageCount = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
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
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        const std::array<std::uint32_t, 2> queueFamilies = {
            graphicsQueueFamily, presentQueueFamily};
        if (graphicsQueueFamily != presentQueueFamily) {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = queueFamilies.data();
        } else {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }
        createInfo.preTransform = capabilities.currentTransform;
        createInfo.compositeAlpha = chooseCompositeAlpha(capabilities.supportedCompositeAlpha);
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        createInfo.oldSwapchain = swapchain;

        VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
        result = vkCreateSwapchainKHR(device, &createInfo, nullptr, &newSwapchain);
        if (result != VK_SUCCESS) {
            return fail(vkFailure("vkCreateSwapchainKHR", result));
        }

        std::uint32_t imageCountReturned = 0;
        result = vkGetSwapchainImagesKHR(device, newSwapchain, &imageCountReturned,
                                         nullptr);
        if (result != VK_SUCCESS || imageCountReturned == 0) {
            vkDestroySwapchainKHR(device, newSwapchain, nullptr);
            return fail(
                result != VK_SUCCESS ? vkFailure("vkGetSwapchainImagesKHR", result)
                                     : "vkGetSwapchainImagesKHR returned no images");
        }
        std::vector<VkImage> newImages(imageCountReturned);
        result = vkGetSwapchainImagesKHR(device, newSwapchain, &imageCountReturned,
                                         newImages.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
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
        for (VkImage image : newImages) {
            viewInfo.image = image;
            VkImageView view = VK_NULL_HANDLE;
            result = vkCreateImageView(device, &viewInfo, nullptr, &view);
            if (result != VK_SUCCESS) {
                for (VkImageView created : newViews) {
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
        for (std::size_t i = 0; i < newImages.size(); ++i) {
            VkSemaphore semaphore = VK_NULL_HANDLE;
            result = vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore);
            if (result != VK_SUCCESS) {
                for (VkSemaphore created : newPresentSemaphores) {
                    vkDestroySemaphore(device, created, nullptr);
                }
                for (VkImageView created : newViews) {
                    vkDestroyImageView(device, created, nullptr);
                }
                vkDestroySwapchainKHR(device, newSwapchain, nullptr);
                return fail(vkFailure("vkCreateSemaphore(presentReady)", result));
            }
            newPresentSemaphores.push_back(semaphore);
        }

        VkImage newDepthImage = VK_NULL_HANDLE;
        VkDeviceMemory newDepthMemory = VK_NULL_HANDLE;
        VkImageView newDepthView = VK_NULL_HANDLE;
        VkDeviceSize newDepthMemorySize = 0;
        const VoidResult depthResult = createDepthResources(
            extent, newDepthImage, newDepthMemory, newDepthView,
            newDepthMemorySize);
        if (!depthResult) {
            for (VkSemaphore created : newPresentSemaphores) {
                vkDestroySemaphore(device, created, nullptr);
            }
            for (VkImageView created : newViews) {
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

        // Build the pipeline against the new format before committing the
        // swapchain.  A failed optional triangle pipeline still leaves a valid
        // clear-only renderer.
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
        const VkImage oldDepthImage = std::exchange(depthImage, newDepthImage);
        const VkDeviceMemory oldDepthMemory =
            std::exchange(depthMemory, newDepthMemory);
        const VkImageView oldDepthView = std::exchange(depthImageView, newDepthView);
        depthMemorySize = newDepthMemorySize;
        depthImageInitialized = false;
        deviceMemoryBytes = depthMemorySize;
        destroyTrianglePipeline();
        (void)createTrianglePipeline();
        if (device != VK_NULL_HANDLE) {
            for (VkImageView view : oldViews) {
                if (view != VK_NULL_HANDLE) {
                    vkDestroyImageView(device, view, nullptr);
                }
            }
            for (VkSemaphore semaphore : oldPresentSemaphores) {
                if (semaphore != VK_NULL_HANDLE) {
                    vkDestroySemaphore(device, semaphore, nullptr);
                }
            }
            if (oldSwapchain != VK_NULL_HANDLE) {
                vkDestroySwapchainKHR(device, oldSwapchain, nullptr);
            }
        }
        if (device != VK_NULL_HANDLE) {
            if (oldDepthView != VK_NULL_HANDLE) {
                vkDestroyImageView(device, oldDepthView, nullptr);
            }
            if (oldDepthImage != VK_NULL_HANDLE) {
                vkDestroyImage(device, oldDepthImage, nullptr);
            }
            if (oldDepthMemory != VK_NULL_HANDLE) {
                vkFreeMemory(device, oldDepthMemory, nullptr);
            }
        }
        framebufferResized = false;
        return ok();
    }

    [[nodiscard]] VoidResult recreateSwapchain() {
        if (device == VK_NULL_HANDLE || surface == VK_NULL_HANDLE) {
            return fail("Cannot recreate swapchain before device/surface creation");
        }
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        if (width <= 0 || height <= 0) {
            return ok();
        }
        const VkResult waitResult = vkDeviceWaitIdle(device);
        if (waitResult != VK_SUCCESS) {
            if (waitResult == VK_ERROR_DEVICE_LOST) {
                deviceLost = true;
            }
            return fail(vkFailure("vkDeviceWaitIdle(recreateSwapchain)",
                                             waitResult));
        }
        requestedExtent = {static_cast<std::uint32_t>(width),
                           static_cast<std::uint32_t>(height)};
        return createSwapchain();
    }

    [[nodiscard]] VoidResult createTrianglePipeline();
    void destroyTrianglePipeline() noexcept {
        if (device == VK_NULL_HANDLE) {
            trianglePipeline = VK_NULL_HANDLE;
            trianglePipelineLayout = VK_NULL_HANDLE;
            triangleVertexShader = VK_NULL_HANDLE;
            triangleFragmentShader = VK_NULL_HANDLE;
            return;
        }
        if (trianglePipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, trianglePipeline, nullptr);
            trianglePipeline = VK_NULL_HANDLE;
        }
        if (trianglePipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, trianglePipelineLayout, nullptr);
            trianglePipelineLayout = VK_NULL_HANDLE;
        }
        if (triangleVertexShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device, triangleVertexShader, nullptr);
            triangleVertexShader = VK_NULL_HANDLE;
        }
        if (triangleFragmentShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device, triangleFragmentShader, nullptr);
            triangleFragmentShader = VK_NULL_HANDLE;
        }
    }

    [[nodiscard]] VoidResult recordFrame(FrameContext& frame, std::uint32_t imageIndex,
                                     const FramePacket& packet);

    [[nodiscard]] FrameStats render(const FramePacket& packet) {
        FrameStats stats{};
        stats.quality.rayQueryEnabled = rayQueryEnabled;
        stats.deviceMemoryBytes = static_cast<std::uint64_t>(deviceMemoryBytes);
        const auto begin = std::chrono::steady_clock::now();
        int framebufferWidth = 0;
        int framebufferHeight = 0;
        if (window != nullptr) {
            glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
        }
        if (!initialized || device == VK_NULL_HANDLE || deviceLost) {
            if (initialized && !deviceLost && swapchain == VK_NULL_HANDLE &&
                framebufferWidth > 0 && framebufferHeight > 0) {
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
        if (framebufferWidth <= 0 || framebufferHeight <= 0) {
            framebufferResized = true;
            stats.minimized = true;
            stats.cpuFrameMs = elapsedMilliseconds(begin);
            return stats;
        }

        if (framebufferResized || swapchain == VK_NULL_HANDLE) {
            const VoidResult resizeResult = recreateSwapchain();
            if (!resizeResult) {
                setError(resizeResult.error().describe());
                stats.deviceLost = deviceLost;
                stats.cpuFrameMs = elapsedMilliseconds(begin);
                return stats;
            }
            lastError.clear();
            stats.recreatedSwapchain = !swapchainImageViews.empty();
            if (swapchain == VK_NULL_HANDLE || swapchainExtent.width == 0 ||
                swapchainExtent.height == 0) {
                stats.minimized = true;
                stats.cpuFrameMs = elapsedMilliseconds(begin);
                return stats;
            }
        }

        auto& frame = frames[currentFrame];
        VkResult result = vkWaitForFences(device, 1, &frame.fence, VK_TRUE,
                                          std::numeric_limits<std::uint64_t>::max());
        if (result != VK_SUCCESS) {
            setError(vkFailure("vkWaitForFences", result));
            if (result == VK_ERROR_DEVICE_LOST) {
                deviceLost = true;
            }
            fatalError = true;
            initialized = false;
            stats.deviceLost = deviceLost;
            stats.fatalError = true;
            stats.cpuFrameMs = elapsedMilliseconds(begin);
            return stats;
        }
        if (timestampsEnabled && frame.submitted) {
            std::array<std::uint64_t, 2> timestampValues{};
            result = vkGetQueryPoolResults(
                device, timestampPool, frame.queryBase, 2,
                sizeof(timestampValues), timestampValues.data(),
                sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
            if (result == VK_SUCCESS) {
                const std::uint64_t validMask = presentTimestampValidBits >= 64
                                                    ? ~std::uint64_t{0}
                                                    : ((std::uint64_t{1}
                                                        << presentTimestampValidBits) -
                                                       1u);
                const std::uint64_t startTimestamp = timestampValues[0] & validMask;
                const std::uint64_t endTimestamp = timestampValues[1] & validMask;
                const std::uint64_t elapsedTicks =
                    (endTimestamp - startTimestamp) & validMask;
                stats.gpuFrameMs = static_cast<double>(elapsedTicks) *
                                   static_cast<double>(timestampPeriod) / 1'000'000.0;
            }
        }
        frame.submitted = false;

        result = vkAcquireNextImageKHR(device, swapchain,
                                       std::numeric_limits<std::uint64_t>::max(),
                                       frame.imageAvailable, VK_NULL_HANDLE,
                                       &stats.swapchainImageIndex);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            framebufferResized = true;
            const VoidResult recreateResult = recreateSwapchain();
            if (!recreateResult) {
                setError(recreateResult.error().describe());
            } else {
                lastError.clear();
            }
            stats.recreatedSwapchain = true;
            stats.cpuFrameMs = elapsedMilliseconds(begin);
            return stats;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            setError(vkFailure("vkAcquireNextImageKHR", result));
            if (result == VK_ERROR_DEVICE_LOST) {
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
        if (stats.swapchainImageIndex >= presentReadySemaphores.size()) {
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
        result = vkResetCommandPool(device, frame.commandPool, 0);
        if (result != VK_SUCCESS) {
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
        if (!recordResult) {
            setError(recordResult.error().describe());
            stats.deviceLost = deviceLost;
            initialized = false;
            fatalError = true;
            stats.fatalError = true;
            stats.cpuFrameMs = elapsedMilliseconds(begin);
            return stats;
        }

        result = vkResetFences(device, 1, &frame.fence);
        if (result != VK_SUCCESS) {
            setError(vkFailure("vkResetFences", result));
            stats.deviceLost = result == VK_ERROR_DEVICE_LOST;
            deviceLost = deviceLost || stats.deviceLost;
            initialized = false;
            fatalError = true;
            stats.fatalError = true;
            stats.cpuFrameMs = elapsedMilliseconds(begin);
            return stats;
        }

        const std::uint64_t signalValue = nextTimelineValue++;
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
        signalInfos[0].semaphore =
            presentReadySemaphores[stats.swapchainImageIndex];
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
        submitInfo.signalSemaphoreInfoCount =
            static_cast<std::uint32_t>(signalInfos.size());
        submitInfo.pSignalSemaphoreInfos = signalInfos.data();
        result = vkQueueSubmit2(graphicsQueue, 1, &submitInfo, frame.fence);
        if (result != VK_SUCCESS) {
            setError(vkFailure("vkQueueSubmit2", result));
            deviceLost = deviceLost || result == VK_ERROR_DEVICE_LOST;
            initialized = false;
            fatalError = true;
            stats.deviceLost = deviceLost;
            stats.fatalError = true;
            stats.cpuFrameMs = elapsedMilliseconds(begin);
            return stats;
        }
        frame.timelineValue = signalValue;
        frame.submitted = true;
        swapchainImageInitialized[stats.swapchainImageIndex] = true;
        depthImageInitialized = true;

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores =
            &presentReadySemaphores[stats.swapchainImageIndex];
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain;
        presentInfo.pImageIndices = &stats.swapchainImageIndex;
        result = vkQueuePresentKHR(presentQueue, &presentInfo);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            framebufferResized = true;
            stats.suboptimal = true;
        } else if (result != VK_SUCCESS) {
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

    static double elapsedMilliseconds(
        const std::chrono::steady_clock::time_point& begin) noexcept {
        const auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(now - begin).count();
    }
};

VoidResult Renderer::Impl::createTrianglePipeline() {
    if (device == VK_NULL_HANDLE || swapchainFormat == VK_FORMAT_UNDEFINED ||
        swapchainExtent.width == 0 || swapchainExtent.height == 0) {
        return ok();
    }

    VkShaderModuleCreateInfo shaderInfo{};
    shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderInfo.codeSize = kTriangleVertexSpirv.size() * sizeof(std::uint32_t);
    shaderInfo.pCode = kTriangleVertexSpirv.data();
    VkResult result = vkCreateShaderModule(device, &shaderInfo, nullptr,
                                           &triangleVertexShader);
    if (result != VK_SUCCESS) {
        triangleVertexShader = VK_NULL_HANDLE;
        return fail(vkFailure("vkCreateShaderModule(vertex)", result));
    }
    shaderInfo.codeSize = kTriangleFragmentSpirv.size() * sizeof(std::uint32_t);
    shaderInfo.pCode = kTriangleFragmentSpirv.data();
    result = vkCreateShaderModule(device, &shaderInfo, nullptr,
                                  &triangleFragmentShader);
    if (result != VK_SUCCESS) {
        vkDestroyShaderModule(device, triangleVertexShader, nullptr);
        triangleVertexShader = VK_NULL_HANDLE;
        triangleFragmentShader = VK_NULL_HANDLE;
        return fail(vkFailure("vkCreateShaderModule(fragment)", result));
    }

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    result = vkCreatePipelineLayout(device, &layoutInfo, nullptr,
                                    &trianglePipelineLayout);
    if (result != VK_SUCCESS) {
        vkDestroyShaderModule(device, triangleVertexShader, nullptr);
        vkDestroyShaderModule(device, triangleFragmentShader, nullptr);
        triangleVertexShader = VK_NULL_HANDLE;
        triangleFragmentShader = VK_NULL_HANDLE;
        return fail(vkFailure("vkCreatePipelineLayout(triangle)", result));
    }

    VkPipelineShaderStageCreateInfo vertexStage{};
    vertexStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertexStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertexStage.module = triangleVertexShader;
    vertexStage.pName = "main";
    VkPipelineShaderStageCreateInfo fragmentStage{};
    fragmentStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragmentStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragmentStage.module = triangleFragmentShader;
    fragmentStage.pName = "main";
    const std::array<VkPipelineShaderStageCreateInfo, 2> stages = {vertexStage,
                                                                    fragmentStage};

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.depthClampEnable = VK_FALSE;
    rasterization.rasterizerDiscardEnable = VK_FALSE;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.depthBiasEnable = VK_FALSE;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    multisample.sampleShadingEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = VK_FALSE;
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                     VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT |
                                     VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blending{};
    blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blending.logicOpEnable = VK_FALSE;
    blending.logicOp = VK_LOGIC_OP_COPY;
    blending.attachmentCount = 1;
    blending.pAttachments = &blendAttachment;

    const std::array<VkDynamicState, 2> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT,
                                                          VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &swapchainFormat;
    renderingInfo.depthAttachmentFormat = depthFormat;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    // Reversed-Z: clear to zero (farthest) and retain the nearest fragment.
    depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &renderingInfo;
    pipelineInfo.stageCount = static_cast<std::uint32_t>(stages.size());
    pipelineInfo.pStages = stages.data();
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterization;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &blending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = trianglePipelineLayout;
    pipelineInfo.renderPass = VK_NULL_HANDLE;
    pipelineInfo.subpass = 0;

    result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                       nullptr, &trianglePipeline);
    if (result != VK_SUCCESS) {
        trianglePipeline = VK_NULL_HANDLE;
        vkDestroyPipelineLayout(device, trianglePipelineLayout, nullptr);
        trianglePipelineLayout = VK_NULL_HANDLE;
        vkDestroyShaderModule(device, triangleVertexShader, nullptr);
        vkDestroyShaderModule(device, triangleFragmentShader, nullptr);
        triangleVertexShader = VK_NULL_HANDLE;
        triangleFragmentShader = VK_NULL_HANDLE;
        return fail(vkFailure("vkCreateGraphicsPipelines(triangle)", result));
    }
    return ok();
}

VoidResult Renderer::Impl::recordFrame(FrameContext& frame, std::uint32_t imageIndex,
                                   const FramePacket& packet) {
    (void)packet; // Scene uploads are introduced by the next milestone.
    if (imageIndex >= swapchainImages.size() ||
        imageIndex >= swapchainImageViews.size()) {
        return fail("Acquired swapchain image index is out of range");
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkResult result = vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);
    if (result != VK_SUCCESS) {
        deviceLost = deviceLost || result == VK_ERROR_DEVICE_LOST;
        return fail(vkFailure("vkBeginCommandBuffer", result));
    }

    if (timestampsEnabled) {
        vkCmdResetQueryPool(frame.commandBuffer, timestampPool, frame.queryBase, 2);
        vkCmdWriteTimestamp2(frame.commandBuffer,
                             VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                             timestampPool, frame.queryBase);
    }

    const bool initializedImage = swapchainImageInitialized[imageIndex];
    VkImageMemoryBarrier2 acquireBarrier{};
    acquireBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    acquireBarrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    acquireBarrier.srcAccessMask = VK_ACCESS_2_NONE;
    acquireBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    acquireBarrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    acquireBarrier.oldLayout = initializedImage ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                                : VK_IMAGE_LAYOUT_UNDEFINED;
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

    if (depthImage == VK_NULL_HANDLE || depthImageView == VK_NULL_HANDLE) {
        return fail("Depth resources are not available for the active swapchain",
                    Halcyon::ErrorCode::InvalidState);
    }
    if (!depthImageInitialized) {
        VkImageMemoryBarrier2 depthBarrier{};
        depthBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        depthBarrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        depthBarrier.srcAccessMask = VK_ACCESS_2_NONE;
        depthBarrier.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                    VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        depthBarrier.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depthBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
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
    }

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
    if (trianglePipeline != VK_NULL_HANDLE) {
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
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          trianglePipeline);
        vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
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

    if (timestampsEnabled) {
        vkCmdWriteTimestamp2(frame.commandBuffer,
                             VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                             timestampPool, frame.queryBase + 1);
    }
    result = vkEndCommandBuffer(frame.commandBuffer);
    if (result != VK_SUCCESS) {
        deviceLost = deviceLost || result == VK_ERROR_DEVICE_LOST;
        return fail(vkFailure("vkEndCommandBuffer", result));
    }
    return ok();
}

Renderer::Renderer() noexcept : impl_(new (std::nothrow) Impl{}) {}

Renderer::~Renderer() {
    if (impl_ != nullptr) {
        impl_->cleanup();
        delete impl_;
        impl_ = nullptr;
    }
}

Renderer::Renderer(Renderer&& other) noexcept : impl_(other.impl_) {
    other.impl_ = nullptr;
}

Renderer& Renderer::operator=(Renderer&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (impl_ != nullptr) {
        impl_->cleanup();
        delete impl_;
    }
    impl_ = other.impl_;
    other.impl_ = nullptr;
    return *this;
}

Halcyon::Result<void> Renderer::initialize(GLFWwindow* window,
                                            const RendererConfig& config) {
    if (impl_ == nullptr) {
        return Halcyon::Result<void>::failure(
            Halcyon::Error{Halcyon::ErrorCode::OutOfMemory,
                           "failed to allocate Vulkan renderer state"});
    }
    impl_->cleanup();
    impl_->lastError.clear();
    if (window == nullptr) {
        impl_->setError("Renderer::initialize received a null GLFWwindow");
        return Halcyon::Result<void>::failure(
            Halcyon::Error{Halcyon::ErrorCode::InvalidArgument,
                           impl_->lastError, "Vulkan renderer initialization"});
    }
    impl_->config = config;
    impl_->window = window;
    impl_->requestedExtent = config.initialExtent;
    if (impl_->config.targetFrameTimeMs <= 0.0f) {
        impl_->config.targetFrameTimeMs = 16.667f;
    }
    try {
        VoidResult result = impl_->createInstance();
        if (!result) {
            impl_->setError(result.error().describe());
            impl_->cleanup();
            return result;
        }
        result = impl_->createSurface();
        if (!result) {
            impl_->setError(result.error().describe());
            impl_->cleanup();
            return result;
        }
        result = impl_->pickPhysicalDevice();
        if (!result) {
            impl_->setError(result.error().describe());
            impl_->cleanup();
            return result;
        }
        result = impl_->createDevice();
        if (!result) {
            impl_->setError(result.error().describe());
            impl_->cleanup();
            return result;
        }
        result = impl_->createTimelineSemaphore();
        if (!result) {
            impl_->setError(result.error().describe());
            impl_->cleanup();
            return result;
        }
        result = impl_->createFrameResources();
        if (!result) {
            impl_->setError(result.error().describe());
            impl_->cleanup();
            return result;
        }
        int framebufferWidth = 0;
        int framebufferHeight = 0;
        if (window != nullptr) {
            glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
        }
        if (framebufferWidth > 0 && framebufferHeight > 0) {
            impl_->requestedExtent = {static_cast<std::uint32_t>(framebufferWidth),
                                      static_cast<std::uint32_t>(framebufferHeight)};
        }
        result = impl_->createSwapchain();
        if (!result) {
            impl_->setError(result.error().describe());
            impl_->cleanup();
            return result;
        }
        impl_->initialized = true;
        impl_->framebufferResized = false;
        return Halcyon::Result<void>::success();
    } catch (const std::exception& exception) {
        impl_->setError(exception.what());
        impl_->cleanup();
        return Halcyon::Result<void>::failure(
            Halcyon::Error{Halcyon::ErrorCode::Backend, impl_->lastError,
                           "Vulkan renderer initialization"});
    } catch (...) {
        impl_->setError("unknown exception during Vulkan renderer initialization");
        impl_->cleanup();
        return Halcyon::Result<void>::failure(
            Halcyon::Error{Halcyon::ErrorCode::Backend, impl_->lastError,
                           "Vulkan renderer initialization"});
    }
}

FrameStats Renderer::render(const FramePacket& packet) {
    if (impl_ == nullptr) {
        FrameStats stats{};
        stats.deviceLost = true;
        stats.fatalError = true;
        return stats;
    }
    return impl_->render(packet);
}

Halcyon::Result<void> Renderer::resize(Extent2D extent) {
    if (impl_ == nullptr) {
        return Halcyon::Result<void>::failure(
            Halcyon::Error{Halcyon::ErrorCode::InvalidState,
                           "renderer state is not allocated"});
    }
    impl_->requestedExtent = extent;
    impl_->framebufferResized = true;
    // Swapchain recreation is deliberately deferred to render().  GLFW can
    // invoke resize callbacks while the framebuffer is transiently zero-sized
    // or while the platform is still processing its window event.
    return Halcyon::Result<void>::success();
}

void Renderer::shutdown() noexcept {
    if (impl_ != nullptr) {
        impl_->cleanup();
    }
}

const Capabilities& Renderer::capabilities() const noexcept {
    static const Capabilities empty{};
    return impl_ != nullptr ? impl_->caps : empty;
}

const std::string& Renderer::lastError() const noexcept {
    static const std::string empty;
    return impl_ != nullptr ? impl_->lastError : empty;
}

bool Renderer::initialized() const noexcept {
    return impl_ != nullptr && impl_->initialized;
}

VkInstance Renderer::instance() const noexcept {
    return impl_ != nullptr ? impl_->instance : VK_NULL_HANDLE;
}

VkPhysicalDevice Renderer::physicalDevice() const noexcept {
    return impl_ != nullptr ? impl_->physicalDevice : VK_NULL_HANDLE;
}

VkDevice Renderer::device() const noexcept {
    return impl_ != nullptr ? impl_->device : VK_NULL_HANDLE;
}

VkQueue Renderer::graphicsQueue() const noexcept {
    return impl_ != nullptr ? impl_->graphicsQueue : VK_NULL_HANDLE;
}

VkQueue Renderer::presentQueue() const noexcept {
    return impl_ != nullptr ? impl_->presentQueue : VK_NULL_HANDLE;
}

} // namespace Halcyon::Vulkan
