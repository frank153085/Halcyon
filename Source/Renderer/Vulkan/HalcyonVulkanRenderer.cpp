#include "HalcyonVulkanRenderer.h"

#include "Core/Profiler.h"
#include "GpuAllocator.h"
#include "GpuUploader.h"
#include "VulkanBindlessTable.h"
#include "VulkanCommon.h"
#include "VulkanDevice.h"
#include "VulkanFrameContext.h"
#include "VulkanFrameGraphProvider.h"
#include "VulkanM3FrameResources.h"
#include "VulkanPipeline.h"
#include "VulkanSceneResources.h"
#include "VulkanSwapchain.h"
#include "../Graph/FrameGraph.h"
#include "../Graph/BarrierPlanner.h"

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
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#undef STB_IMAGE_WRITE_IMPLEMENTATION
#include <filesystem>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <new>
#include <limits>
#include <utility>
#include <vector>

namespace Halcyon::Vulkan
{
namespace Graph = Halcyon::Renderer::Graph;
#if HALCYON_BUILD_EXPERIMENTAL_M2
#endif
namespace
{

[[nodiscard]] std::uint16_t floatToHalf(float value) noexcept
{
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const std::uint32_t sign = (bits >> 16u) & 0x8000u;
    const std::uint32_t exponent = (bits >> 23u) & 0xffu;
    std::uint32_t mantissa = bits & 0x7fffffu;
    if (exponent == 0xffu)
    {
        return static_cast<std::uint16_t>(sign | (mantissa == 0u ? 0x7c00u : 0x7e00u));
    }
    const int halfExponent = static_cast<int>(exponent) - 127 + 15;
    if (halfExponent >= 31)
    {
        return static_cast<std::uint16_t>(sign | 0x7c00u);
    }
    if (halfExponent <= 0)
    {
        if (halfExponent < -10)
        {
            return static_cast<std::uint16_t>(sign);
        }
        mantissa |= 0x800000u;
        const std::uint32_t shift = static_cast<std::uint32_t>(14 - halfExponent);
        const std::uint32_t rounded = (mantissa + (1u << (shift - 1u))) >> shift;
        return static_cast<std::uint16_t>(sign | rounded);
    }
    mantissa += 0x1000u;
    if ((mantissa & 0x800000u) != 0u)
    {
        mantissa = 0;
        if (halfExponent + 1 >= 31)
            return static_cast<std::uint16_t>(sign | 0x7c00u);
        return static_cast<std::uint16_t>(sign |
            (static_cast<std::uint32_t>(halfExponent + 1) << 10u));
    }
    return static_cast<std::uint16_t>(sign |
        (static_cast<std::uint32_t>(halfExponent) << 10u) | (mantissa >> 13u));
}

[[nodiscard]] glm::vec3 cubeDirection(std::uint32_t face, float u, float v) noexcept
{
    const glm::vec3 direction = face == 0u ? glm::vec3{1.0f, -v, -u}
        : face == 1u ? glm::vec3{-1.0f, -v, u}
        : face == 2u ? glm::vec3{u, 1.0f, v}
        : face == 3u ? glm::vec3{u, -1.0f, -v}
        : face == 4u ? glm::vec3{u, -v, 1.0f}
                     : glm::vec3{-u, -v, -1.0f};
    return glm::normalize(direction);
}

[[nodiscard]] glm::vec3 proceduralSky(glm::vec3 direction, float roughness) noexcept
{
    const float skyAmount = glm::smoothstep(-0.08f, 0.18f, direction.y);
    const float zenith = std::pow(std::max(direction.y, 0.0f), 0.35f);
    const glm::vec3 ground{0.018f, 0.014f, 0.012f};
    const glm::vec3 horizon{0.22f, 0.30f, 0.48f};
    const glm::vec3 top{0.025f, 0.075f, 0.19f};
    glm::vec3 radiance = glm::mix(ground, glm::mix(horizon, top, zenith), skyAmount);
    const glm::vec3 sunDirection = glm::normalize(glm::vec3{0.32f, 0.88f, 0.24f});
    const float sunExponent = glm::mix(768.0f, 4.0f, roughness);
    const float sun = std::pow(std::max(glm::dot(direction, sunDirection), 0.0f), sunExponent);
    radiance += glm::vec3{8.0f, 6.2f, 4.2f} * sun * (1.0f - roughness);
    const glm::vec3 average{0.085f, 0.105f, 0.15f};
    return glm::mix(radiance, average, roughness * roughness * 0.82f);
}

void appendRgba16(std::vector<std::uint16_t>& output, const glm::vec3& color)
{
    output.push_back(floatToHalf(color.r));
    output.push_back(floatToHalf(color.g));
    output.push_back(floatToHalf(color.b));
    output.push_back(floatToHalf(1.0f));
}

[[nodiscard]] float radicalInverse(std::uint32_t bits) noexcept
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xaaaaaaaau) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xccccccccu) >> 2u);
    bits = ((bits & 0x0f0f0f0fu) << 4u) | ((bits & 0xf0f0f0f0u) >> 4u);
    bits = ((bits & 0x00ff00ffu) << 8u) | ((bits & 0xff00ff00u) >> 8u);
    return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

[[nodiscard]] glm::vec2 integrateBrdf(float nDotV, float roughness) noexcept
{
    constexpr std::uint32_t sampleCount = 64u;
    constexpr float pi = 3.14159265358979323846f;
    const glm::vec3 view{std::sqrt(std::max(0.0f, 1.0f - nDotV * nDotV)), 0.0f, nDotV};
    float scale = 0.0f;
    float bias = 0.0f;
    const float alpha = std::max(0.002f, roughness * roughness);
    for (std::uint32_t sample = 0; sample < sampleCount; ++sample)
    {
        const float xiX = static_cast<float>(sample) / static_cast<float>(sampleCount);
        const float xiY = radicalInverse(sample);
        const float phi = 2.0f * pi * xiX;
        const float cosTheta = std::sqrt((1.0f - xiY) /
            std::max(1.0f + (alpha * alpha - 1.0f) * xiY, 1.0e-6f));
        const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
        const glm::vec3 halfVector{std::cos(phi) * sinTheta, std::sin(phi) * sinTheta,
            cosTheta};
        const glm::vec3 light = glm::normalize(2.0f * glm::dot(view, halfVector) * halfVector - view);
        const float nDotL = std::max(light.z, 0.0f);
        const float nDotH = std::max(halfVector.z, 0.0f);
        const float vDotH = std::max(glm::dot(view, halfVector), 0.0f);
        if (nDotL <= 0.0f || nDotH <= 0.0f)
            continue;
        const float k = roughness * roughness * 0.5f;
        const float gV = nDotV / std::max(nDotV * (1.0f - k) + k, 1.0e-6f);
        const float gL = nDotL / std::max(nDotL * (1.0f - k) + k, 1.0e-6f);
        const float visibility = gV * gL * vDotH /
            std::max(nDotH * nDotV, 1.0e-6f);
        const float fresnel = std::pow(1.0f - vDotH, 5.0f);
        scale += (1.0f - fresnel) * visibility;
        bias += fresnel * visibility;
    }
    return {scale / static_cast<float>(sampleCount), bias / static_cast<float>(sampleCount)};
}

struct ProceduralIblData
{
    std::vector<std::uint16_t> irradiance;
    std::vector<VkBufferImageCopy> irradianceCopies;
    std::vector<std::uint16_t> prefiltered;
    std::vector<VkBufferImageCopy> prefilteredCopies;
    std::vector<std::uint16_t> brdf;
    std::vector<VkBufferImageCopy> brdfCopies;
};

[[nodiscard]] ProceduralIblData createProceduralIbl()
{
    ProceduralIblData result;
    constexpr std::uint32_t irradianceSize = 32u;
    for (std::uint32_t face = 0; face < 6u; ++face)
    {
        VkBufferImageCopy copy{};
        copy.bufferOffset = result.irradiance.size() * sizeof(std::uint16_t);
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, face, 1};
        copy.imageExtent = {irradianceSize, irradianceSize, 1};
        result.irradianceCopies.push_back(copy);
        for (std::uint32_t y = 0; y < irradianceSize; ++y)
            for (std::uint32_t x = 0; x < irradianceSize; ++x)
            {
                const float u = (2.0f * (static_cast<float>(x) + 0.5f) / irradianceSize) - 1.0f;
                const float v = (2.0f * (static_cast<float>(y) + 0.5f) / irradianceSize) - 1.0f;
                const glm::vec3 direction = cubeDirection(face, u, v);
                const glm::vec3 diffuse = proceduralSky(direction, 0.82f) * 3.14159265f;
                appendRgba16(result.irradiance, diffuse);
            }
    }
    constexpr std::uint32_t baseSize = 64u;
    constexpr std::uint32_t mipCount = 7u;
    for (std::uint32_t mip = 0; mip < mipCount; ++mip)
    {
        const std::uint32_t size = std::max(1u, baseSize >> mip);
        const float roughness = static_cast<float>(mip) / static_cast<float>(mipCount - 1u);
        for (std::uint32_t face = 0; face < 6u; ++face)
        {
            VkBufferImageCopy copy{};
            copy.bufferOffset = result.prefiltered.size() * sizeof(std::uint16_t);
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, face, 1};
            copy.imageExtent = {size, size, 1};
            result.prefilteredCopies.push_back(copy);
            for (std::uint32_t y = 0; y < size; ++y)
                for (std::uint32_t x = 0; x < size; ++x)
                {
                    const float u = (2.0f * (static_cast<float>(x) + 0.5f) / size) - 1.0f;
                    const float v = (2.0f * (static_cast<float>(y) + 0.5f) / size) - 1.0f;
                    appendRgba16(result.prefiltered,
                        proceduralSky(cubeDirection(face, u, v), roughness));
                }
        }
    }
    constexpr std::uint32_t lutSize = 128u;
    VkBufferImageCopy lutCopy{};
    lutCopy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    lutCopy.imageExtent = {lutSize, lutSize, 1};
    result.brdfCopies.push_back(lutCopy);
    result.brdf.reserve(lutSize * lutSize * 2u);
    for (std::uint32_t y = 0; y < lutSize; ++y)
        for (std::uint32_t x = 0; x < lutSize; ++x)
        {
            const glm::vec2 integrated = integrateBrdf(
                (static_cast<float>(x) + 0.5f) / lutSize,
                (static_cast<float>(y) + 0.5f) / lutSize);
            result.brdf.push_back(floatToHalf(integrated.x));
            result.brdf.push_back(floatToHalf(integrated.y));
        }
    return result;
}

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

    VulkanPipeline csmDepthPipeline;
    VulkanPipeline gbufferPipeline;
    VulkanPipeline gbufferDoubleSidedPipeline;
    VulkanPipeline deferredLightingPipeline;
    VulkanPipeline transparentPipeline;
    VulkanPipeline transparentDoubleSidedPipeline;
    VulkanPipeline taaPipeline;
    VulkanPipeline tonemapPipeline;
    VulkanPipeline clusterBuildPipeline;
    VkDescriptorSetLayout m3MaterialLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m3LightingLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m3TaaLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m3ClusterLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m3TonemapLayout = VK_NULL_HANDLE;
    VkDescriptorPool m3DescriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> m3DescriptorPools;
    VkSampler m3LinearSampler = VK_NULL_HANDLE;

    VkExtent2D& requestedExtent = swapchainState.requestedExtent;
    bool& framebufferResized = swapchainState.framebufferResized;
    bool initialized = false;
    bool deviceLost = false;
    bool fatalError = false;
    bool& rayQueryEnabled = deviceState.rayQueryEnabled;
    VkDeviceSize deviceMemoryBytes = 0;
    GpuAllocator gpuAllocator;
    GpuUploader gpuUploader;
    std::vector<BufferAllocation> clusterOverflowReadbacks;
    std::vector<std::vector<BufferAllocation>> frameUploadBuffers;
    VulkanFrameGraphProvider frameGraphProvider;
    VulkanM3FrameResources m3FrameResources;
    VulkanSceneResources sceneResources;
    VulkanBindlessTable bindlessTable;
    OverlayCallback overlayCallback = nullptr;
    std::uint32_t lastPresentedImage = 0;
    bool taaHistoryValid = false;
    bool taaHistoryFlip = false;
    bool taaHistoryInitializedA = false;
    bool taaHistoryInitializedB = false;
    bool iblInitialized = false;
    bool hasRenderedFrame = false;
    std::uint64_t lastFrameIndex = 0;
    std::uint64_t renderSerial = 0;
    std::vector<InstanceData> previousInstances;
    glm::mat4 previousViewProjection{1.0f};
    bool previousPacketValid = false;

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
        csmDepthPipeline.destroy();
        gbufferPipeline.destroy();
        gbufferDoubleSidedPipeline.destroy();
        deferredLightingPipeline.destroy();
        transparentPipeline.destroy();
        transparentDoubleSidedPipeline.destroy();
        taaPipeline.destroy();
        tonemapPipeline.destroy();
        clusterBuildPipeline.destroy();
        m3FrameResources.reset();
        frameGraphProvider.recreatePersistent();
        swapchainState.cleanup();
    }

    void cleanupM3Descriptors() noexcept
    {
        if (device != VK_NULL_HANDLE)
        {
            for (auto pool : m3DescriptorPools)
                if (pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, pool, nullptr);
            if (m3LinearSampler != VK_NULL_HANDLE)
                vkDestroySampler(device, m3LinearSampler, nullptr);
            if (m3MaterialLayout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(device, m3MaterialLayout, nullptr);
            if (m3LightingLayout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(device, m3LightingLayout, nullptr);
            if (m3TaaLayout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(device, m3TaaLayout, nullptr);
            if (m3ClusterLayout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(device, m3ClusterLayout, nullptr);
            if (m3TonemapLayout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(device, m3TonemapLayout, nullptr);
        }
        m3DescriptorPool = VK_NULL_HANDLE;
        m3DescriptorPools.clear();
        m3LinearSampler = VK_NULL_HANDLE;
        m3MaterialLayout = VK_NULL_HANDLE;
        m3LightingLayout = VK_NULL_HANDLE;
        m3TaaLayout = VK_NULL_HANDLE;
        m3ClusterLayout = VK_NULL_HANDLE;
        m3TonemapLayout = VK_NULL_HANDLE;
    }

    void cleanup() noexcept
    {
        if (device != VK_NULL_HANDLE)
        {
            // Waiting is best effort during error cleanup; every child object
            // is still destroyed even when the device has already been lost.
            (void)vkDeviceWaitIdle(device);
            cleanupSwapchain();
            cleanupM3Descriptors();
            frameGraphProvider.shutdown();
            sceneResources.cleanup();
            bindlessTable.shutdown();
            for (auto& readback : clusterOverflowReadbacks)
                gpuAllocator.destroy(readback);
            clusterOverflowReadbacks.clear();
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
        lastPresentedImage = 0;
        taaHistoryValid = false;
        taaHistoryFlip = false;
        taaHistoryInitializedA = false;
        taaHistoryInitializedB = false;
        iblInitialized = false;
        hasRenderedFrame = false;
        lastFrameIndex = 0;
        renderSerial = 0;
        previousInstances.clear();
        previousViewProjection = glm::mat4{1.0f};
        previousPacketValid = false;
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
        clusterOverflowReadbacks.clear();
        clusterOverflowReadbacks.reserve(frames.size());
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
                return allocation.error();
            }
            clusterOverflowReadbacks.push_back(allocation.value());
        }
        return ok();
    }

    [[nodiscard]] VoidResult createSwapchain()
    {
        const VoidResult result = swapchainState.create();
        if (!result)
        {
            return result;
        }
        const VoidResult resourceResult = m3FrameResources.recreate(swapchainExtent);
        if (!resourceResult)
        {
            return resourceResult;
        }
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
        // FrameGraph persistent images (TAA history and procedural IBL) are
        // keyed by their descriptor, which includes the swapchain extent for
        // history targets.  The swapchain recreation waits for the device,
        // so it is now safe to retire and rebuild those native allocations
        // before the next graph materialization.  Keeping the old cache here
        // would alias a resized history image with the previous extent.
        frameGraphProvider.recreatePersistent();
        const VoidResult resourceResult = m3FrameResources.recreate(swapchainExtent);
        if (!resourceResult)
        {
            return resourceResult;
        }
        const VoidResult pipelineResult = createGraphicsPipeline();
        deviceMemoryBytes = gpuAllocator.allocatedBytes();
        // A swapchain resize changes the sampling footprint, so any temporal
        // history must be discarded before the next rendered frame.
        taaHistoryValid = false;
        taaHistoryInitializedA = false;
        taaHistoryInitializedB = false;
        iblInitialized = false;
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
        const std::array<VkFormat, 4> gbufferFormats = {
            VK_FORMAT_R8G8B8A8_SRGB,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_FORMAT_R16G16_SFLOAT};
        GraphicsPipelineDesc gbufferDesc{};
        gbufferDesc.colorFormats = gbufferFormats;
        gbufferDesc.depthFormat = depthFormat;
        gbufferDesc.cullMode = VK_CULL_MODE_BACK_BIT;
        gbufferDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&m3MaterialLayout, 1};
        gbufferDesc.descriptorBindings = materialAbi;
        const std::array<VkPushConstantRange, 1> gbufferPushRanges = {
            VkPushConstantRange{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(M3PushConstants)}};
        gbufferDesc.pushConstants = gbufferPushRanges;
        gbufferDesc.vertexShader = "pbr.vert.spv";
        gbufferDesc.fragmentShader = "gbuffer.frag.spv";
        const auto gbufferResult = gbufferPipeline.createGraphics(device, gbufferDesc);
        if (!gbufferResult) return gbufferResult;
        GraphicsPipelineDesc gbufferDoubleDesc = gbufferDesc;
        gbufferDoubleDesc.cullMode = VK_CULL_MODE_NONE;
        const auto gbufferDoubleResult = gbufferDoubleSidedPipeline.createGraphics(device, gbufferDoubleDesc);
        if (!gbufferDoubleResult) return gbufferDoubleResult;

        const std::array<VkFormat, 1> hdrFormat = {VK_FORMAT_R16G16B16A16_SFLOAT};
        GraphicsPipelineDesc deferredDesc{};
        deferredDesc.colorFormats = hdrFormat;
        deferredDesc.depthFormat = VK_FORMAT_UNDEFINED;
        deferredDesc.depthTest = false;
        deferredDesc.depthWrite = false;
        deferredDesc.cullMode = VK_CULL_MODE_NONE;
        deferredDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&m3LightingLayout, 1};
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
        transparentDesc.vertexShader = "pbr.vert.spv";
        transparentDesc.fragmentShader = "forward_transparent.frag.spv";
        transparentDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&m3MaterialLayout, 1};
        transparentDesc.descriptorBindings = materialAbi;
        const std::array<VkPushConstantRange, 1> transparentPushRange = {
            VkPushConstantRange{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(M3TransparentPushConstants)}};
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
        tonemapDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&m3TonemapLayout, 1};
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
        taaDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&m3TaaLayout, 1};
        taaDesc.descriptorBindings = taaAbi;
        const std::array<VkPushConstantRange, 1> taaPushRange = {
            VkPushConstantRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16}};
        taaDesc.pushConstants = taaPushRange;
        const auto taaResult = taaPipeline.createCompute(device, taaDesc);
        if (!taaResult) return taaResult;
        ComputePipelineDesc clusterDesc{};
        clusterDesc.shader = "cluster_build.comp.spv";
        clusterDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&m3ClusterLayout, 1};
        clusterDesc.descriptorBindings = clusterAbi;
        const std::array<VkPushConstantRange, 1> clusterPushRange = {
            VkPushConstantRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32}};
        clusterDesc.pushConstants = clusterPushRange;
        const auto clusterResult = clusterBuildPipeline.createCompute(device, clusterDesc);
        if (!clusterResult) return clusterResult;
        return ok();
    }

    [[nodiscard]] VoidResult createM3Descriptors()
    {
        if (m3LightingLayout != VK_NULL_HANDLE)
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
        auto result = createLayout(materialBindings, m3MaterialLayout);
        if (!result) return result;
        std::array<VkDescriptorSetLayoutBinding, 13> lightingBindings{};
        for (std::uint32_t i = 0; i < 8; ++i)
            lightingBindings[i] = {i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        lightingBindings[8] = {10, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        lightingBindings[9] = {20, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        lightingBindings[10] = {21, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        lightingBindings[11] = {22, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        lightingBindings[12] = {23, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        result = createLayout(lightingBindings, m3LightingLayout);
        if (!result) return result;
        std::array<VkDescriptorSetLayoutBinding, 5> taaBindings = {
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{10, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{20, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        result = createLayout(taaBindings, m3TaaLayout);
        if (!result) return result;
        std::array<VkDescriptorSetLayoutBinding, 5> clusterBindings = {
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        result = createLayout(clusterBindings, m3ClusterLayout);
        if (!result) return result;
        const std::array<VkDescriptorSetLayoutBinding, 2> tonemapBindings = {
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{10, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
        result = createLayout(tonemapBindings, m3TonemapLayout);
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
        if (vkCreateSampler(device, &samplerInfo, nullptr, &m3LinearSampler) != VK_SUCCESS)
            return fail("failed to create M3 linear sampler");
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
        m3DescriptorPools.reserve(poolCount);
        for (std::uint32_t i = 0; i < poolCount; ++i)
        {
            VkDescriptorPool pool = VK_NULL_HANDLE;
            if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS)
            {
                cleanupM3Descriptors();
                return fail("failed to create M3 descriptor pool");
            }
            m3DescriptorPools.push_back(pool);
        }
        m3DescriptorPool = m3DescriptorPools.front();
        return ok();
    }

    [[nodiscard]] VoidResult recordM3Frame(
        FrameContext& frame, std::uint32_t imageIndex, const FramePacket& packet);

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
        if (!initialized || device == VK_NULL_HANDLE || swapchain == VK_NULL_HANDLE ||
            swapchainExtent.width == 0 || swapchainExtent.height == 0 ||
            lastPresentedImage >= swapchainImages.size())
        {
            return fail(
                "No rendered frame is available for screenshot", Halcyon::ErrorCode::InvalidState);
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
        if (!bufferResult)
        {
            return bufferResult.error();
        }
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
        if (result == VK_SUCCESS)
        {
            vkCmdPipelineBarrier2(command, &dep);
        }
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {swapchainExtent.width, swapchainExtent.height, 1};
        if (result == VK_SUCCESS)
        {
            vkCmdCopyImageToBuffer(command,
                swapchainImages[lastPresentedImage],
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                readback.buffer,
                1,
                &region);
        }
        VkImageMemoryBarrier2 back = toCopy;
        back.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        back.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        back.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
        back.dstAccessMask = VK_ACCESS_2_NONE;
        back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        back.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        dep.pImageMemoryBarriers = &back;
        if (result == VK_SUCCESS)
        {
            vkCmdPipelineBarrier2(command, &dep);
        }
        if (result == VK_SUCCESS)
        {
            result = vkEndCommandBuffer(command);
        }
        if (result == VK_SUCCESS)
        {
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &command;
            result = vkQueueSubmit(graphicsQueue, 1, &submit, VK_NULL_HANDLE);
            if (result == VK_SUCCESS)
            {
                result = vkQueueWaitIdle(graphicsQueue);
            }
        }
        if (command != VK_NULL_HANDLE)
        {
            vkFreeCommandBuffers(device, frames.front().commandPool, 1, &command);
        }
        if (result != VK_SUCCESS)
        {
            gpuAllocator.destroy(readback);
            return fail(vkFailure("screenshot readback", result));
        }
        auto bytes = gpuAllocator.readBuffer(readback, 0, size);
        gpuAllocator.destroy(readback);
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
        // Convert stable SceneDatabase slots to dense GPU indices once at the
        // renderer boundary. Draw recording only consumes these contiguous
        // indices; no Vulkan lookup ever depends on Handle::index().
        std::vector<InstanceData> remappedInstances;
        remappedInstances.reserve(packet.instances.size());
        for (const auto& source : packet.instances)
        {
            const auto meshIndex = sceneResources.meshDenseIndex(source.meshId);
            const auto materialIndex = sceneResources.materialDenseIndex(source.materialId);
            if (meshIndex == std::numeric_limits<std::uint32_t>::max() ||
                materialIndex == std::numeric_limits<std::uint32_t>::max())
                continue;
            InstanceData item = source;
            item.meshId = meshIndex;
            item.materialId = materialIndex;
            if (const MeshResource* mesh = sceneResources.mesh(item.meshId);
                mesh != nullptr)
            {
                stats.primitiveCount += mesh->indexCount / 3u;
            }
            remappedInstances.push_back(item);
        }
        const FramePacket effectivePacket{packet.frameIndex, packet.camera,
            std::span<const InstanceData>{remappedInstances}, packet.lights};
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
                }
            }
        }
        if (config.enableClusteredLighting && frame.submitted &&
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

        const VoidResult recordResult = recordM3Frame(frame, stats.swapchainImageIndex, effectivePacket);
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
        stats.executedPasses = frame.passNames;

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
        ++renderSerial;
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
        deviceMemoryBytes = gpuAllocator.allocatedBytes();
        stats.deviceMemoryBytes = static_cast<std::uint64_t>(deviceMemoryBytes);
        stats.quality.rayQueryEnabled = rayQueryEnabled;
        stats.taaHistoryValid = config.enableTaa && taaHistoryValid && hasRenderedFrame &&
                                packet.frameIndex == lastFrameIndex + 1u;
        if (config.enableTaa)
        {
            taaHistoryValid = true;
            hasRenderedFrame = true;
            lastFrameIndex = packet.frameIndex;
        }
        previousInstances = remappedInstances;
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

VoidResult Renderer::Impl::recordM3Frame(
    FrameContext& frame, std::uint32_t imageIndex, const FramePacket& packet)
{
    HALCYON_PROFILE_SCOPE("Renderer::recordM3Frame");
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
            2u + frameContext.maxPassCount * 2u);
        vkCmdWriteTimestamp2(frame.commandBuffer, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            frameContext.timestampPool, frame.queryBase);
    }

    struct ImportedTarget
    {
        VkImageView view = VK_NULL_HANDLE;
    } importedTarget{swapchainImageViews[imageIndex]};

    Graph::FrameGraph graph;
    graph.setResourceProvider(&frameGraphProvider);
        if (currentFrame < m3DescriptorPools.size())
        {
            m3DescriptorPool = m3DescriptorPools[currentFrame];
            (void)vkResetDescriptorPool(device, m3DescriptorPool, 0);
        }
    const auto allocateSet = [&](VkDescriptorSetLayout layout) -> VkDescriptorSet
    {
        if (layout == VK_NULL_HANDLE || m3DescriptorPool == VK_NULL_HANDLE)
        {
            setError("M3 descriptor allocation requested with an invalid layout or pool");
            fatalError = true;
            return VK_NULL_HANDLE;
        }
        VkDescriptorSetAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate.descriptorPool = m3DescriptorPool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        const VkResult allocationResult = vkAllocateDescriptorSets(device, &allocate, &set);
        if (allocationResult != VK_SUCCESS)
        {
            setError(vkFailure("vkAllocateDescriptorSets", allocationResult));
            fatalError = true;
            return VK_NULL_HANDLE;
        }
        return set;
    };
    const auto writeSampled = [&](VkDescriptorSet set, std::uint32_t binding, VkImageView view,
                                  VkImageLayout imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        VkDescriptorImageInfo image{VK_NULL_HANDLE, view, imageLayout};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = binding;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    };
    const auto writeSampler = [&](VkDescriptorSet set)
    {
        VkDescriptorImageInfo sampler{m3LinearSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = 10;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        write.pImageInfo = &sampler;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    };
    const auto writeStorage = [&](VkDescriptorSet set, std::uint32_t binding, VkImageView view)
    {
        VkDescriptorImageInfo image{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = binding;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    };
    const auto writeStorageBuffer = [&](VkDescriptorSet set, std::uint32_t binding,
                                         VkBuffer buffer, VkDeviceSize size)
    {
        VkDescriptorBufferInfo info{buffer, 0, size};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = binding;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &info;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    };
    const auto writeUniformBuffer = [&](VkDescriptorSet set, std::uint32_t binding,
                                        VkBuffer buffer, VkDeviceSize size)
    {
        VkDescriptorBufferInfo info{buffer, 0, size};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = binding;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &info;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    };
    const std::uint32_t width = swapchainExtent.width;
    const std::uint32_t height = swapchainExtent.height;
    constexpr std::uint32_t csmResolution = VulkanM3FrameResources::CsmResolution;
    const auto transitionImage = [&](VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                                      VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                                      VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                                      VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                                      std::uint32_t baseLayer = 0, std::uint32_t layerCount = 1)
    {
        if (image == VK_NULL_HANDLE) return;
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = srcStage;
        barrier.srcAccessMask = srcAccess;
        barrier.dstStageMask = dstStage;
        barrier.dstAccessMask = dstAccess;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, baseLayer, layerCount};
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(frame.commandBuffer, &dependency);
    };
    auto declaredResources = m3FrameResources.declare(
        graph, static_cast<std::uint32_t>(packet.lights.size()));
    if (!declaredResources)
    {
        return VoidResult::failure(declaredResources.error());
    }
    auto m3 = declaredResources.value();
    auto shadow = m3.csm;
    auto gbuffer0 = m3.gbuffer0;
    auto gbuffer1 = m3.gbuffer1;
    auto gbuffer2 = m3.gbuffer2;
    auto motion = m3.motion;
    auto depth = m3.depth;
    auto hdr = m3.hdr;
    auto historyA = m3.historyA;
    auto historyB = m3.historyB;
    auto irradiance = m3.irradiance;
    auto prefiltered = m3.prefiltered;
    auto brdfLut = m3.brdfLut;
    auto clusterRanges = m3.clusterRanges;
    auto clusterIndices = m3.clusterIndices;
    auto clusterOverflow = m3.clusterOverflow;
    auto lightBuffer = m3.lights;
    auto clusterCamera = m3.clusterCamera;
    auto shadowConstantsBuffer = m3.shadowConstants;
    const std::uint32_t tileCount = m3.clusterCount;

    Graph::FrameGraphRenderPass::ImportDescriptor swapImport{};
    swapImport.attachments = Graph::FrameGraphAttachmentFlags::Color0;
    swapImport.viewport.width = width;
    swapImport.viewport.height = height;
    swapImport.clearFlags = Graph::FrameGraphAttachmentFlags::Color0;
    auto output = graph.import("Swapchain", swapImport, {&importedTarget});

    std::array<glm::mat4, 4> cascadeMatrices{};
    glm::vec4 cascadeSplits{0.0f};
    {
        const float nearPlane = std::max(1.0e-3f, packet.camera.positionAndNear.w);
        const float farPlane = packet.camera.forwardAndFar.w > nearPlane
                                   ? packet.camera.forwardAndFar.w
                                   : 1000.0f;
        const glm::mat4 invViewProjection = packet.camera.inverseViewProjection;
        std::array<glm::vec3, 8> frustumCorners{};
        std::size_t corner = 0;
        for (float z : {1.0f, 0.0f})
        {
            for (float y : {-1.0f, 1.0f})
            {
                for (float x : {-1.0f, 1.0f})
                {
                    const glm::vec4 clip{x, y, z, 1.0f};
                    const glm::vec4 world = invViewProjection * clip;
                    frustumCorners[corner++] = world.w != 0.0f
                                                    ? glm::vec3(world) / world.w
                                                    : glm::vec3(world);
                }
            }
        }
        glm::vec3 lightDirection{0.35f, 0.85f, 0.2f};
        for (const auto& light : packet.lights)
        {
            if (light.directionAndType[3] > 0.5f && light.directionAndType[3] < 1.5f)
            {
                const glm::vec3 candidate{light.directionAndType[0], light.directionAndType[1],
                    light.directionAndType[2]};
                if (glm::dot(candidate, candidate) > 1.0e-6f)
                    lightDirection = candidate;
                break;
            }
        }
        lightDirection = glm::normalize(lightDirection);
        const glm::vec3 up = std::abs(glm::dot(lightDirection, glm::vec3{0, 1, 0})) > 0.95f
                                 ? glm::vec3{1, 0, 0}
                                 : glm::vec3{0, 1, 0};
        const float lambda = 0.65f;
        float previousSplit = nearPlane;
        for (std::uint32_t cascade = 0; cascade < 4; ++cascade)
        {
            const float p = static_cast<float>(cascade + 1u) / 4.0f;
            const float logarithmic = nearPlane * std::pow(farPlane / nearPlane, p);
            const float split = glm::mix(nearPlane + (farPlane - nearPlane) * p,
                logarithmic, lambda);
            const float startRatio = (previousSplit - nearPlane) / (farPlane - nearPlane);
            const float endRatio = (split - nearPlane) / (farPlane - nearPlane);
            std::array<glm::vec3, 8> sliceCorners{};
            for (std::size_t i = 0; i < 4; ++i)
            {
                const glm::vec3 nearCorner = frustumCorners[i];
                const glm::vec3 farCorner = frustumCorners[i + 4];
                sliceCorners[i] = glm::mix(nearCorner, farCorner, startRatio);
                sliceCorners[i + 4] = glm::mix(nearCorner, farCorner, endRatio);
            }
            glm::vec3 center{0.0f};
            for (const auto& point : sliceCorners) center += point;
            center /= 8.0f;
            float radius = 0.0f;
            for (const auto& point : sliceCorners)
                radius = std::max(radius, glm::length(point - center));
            radius = std::max(radius, 1.0f);
            radius = std::ceil(radius * 16.0f) / 16.0f;
            glm::vec3 lightPosition = center - lightDirection * radius * 2.0f;
            glm::mat4 lightView = glm::lookAtRH(lightPosition, center, up);
            const glm::vec3 centerLight = glm::vec3(lightView * glm::vec4(center, 1.0f));
            const float texelSize = (2.0f * radius) / 2048.0f;
            const glm::vec2 snapped = glm::floor(glm::vec2(centerLight) / texelSize) * texelSize;
            const glm::vec2 delta = snapped - glm::vec2(centerLight);
            center += glm::vec3(lightView[0]) * delta.x + glm::vec3(lightView[1]) * delta.y;
            lightPosition = center - lightDirection * radius * 2.0f;
            lightView = glm::lookAtRH(lightPosition, center, up);
            const float depthNear = 0.1f;
            const float depthFar = radius * 4.0f + 10.0f;
            cascadeMatrices[cascade] = glm::orthoRH_ZO(-radius, radius, -radius, radius,
                depthFar, depthNear) * lightView;
            cascadeSplits[cascade] = split;
            previousSplit = split;
        }
    }

    const auto drawScene = [&](VkPipelineLayout layout,
                                bool shadowPass, bool transparentPass,
                                std::uint32_t cascade = 0u)
    {
        std::vector<const InstanceData*> drawItems;
        drawItems.reserve(packet.instances.size());
        for (const auto& instance : packet.instances)
            drawItems.push_back(&instance);
        if (transparentPass)
        {
            const glm::vec3 cameraPosition = glm::vec3(packet.camera.positionAndNear);
            std::stable_sort(drawItems.begin(), drawItems.end(), [&](const InstanceData* a,
                const InstanceData* b)
            {
                const glm::vec3 aPosition{a->transform[12], a->transform[13], a->transform[14]};
                const glm::vec3 bPosition{b->transform[12], b->transform[13], b->transform[14]};
                return glm::dot(aPosition - cameraPosition, aPosition - cameraPosition) >
                       glm::dot(bPosition - cameraPosition, bPosition - cameraPosition);
            });
        }
        for (const InstanceData* instancePointer : drawItems)
        {
            const auto& instance = *instancePointer;
            const bool isTransparent = (instance.flags & 1u) != 0u;
            const bool castsShadow = (instance.flags & (1u << 2u)) != 0u;
            if (shadowPass && (!castsShadow || isTransparent)) continue;
            if (!shadowPass && transparentPass != isTransparent) continue;
            const MeshResource* mesh = sceneResources.mesh(instance.meshId);
            if (mesh == nullptr || mesh->indexCount == 0) continue;
            VkBuffer vertex = mesh->vertexBuffer.buffer;
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &vertex, &offset);
            vkCmdBindIndexBuffer(frame.commandBuffer, mesh->indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
            VkPipelineLayout drawLayout = layout;
            if (!shadowPass)
            {
                const bool doubleSided = (instance.flags & (1u << 1u)) != 0u;
                if (doubleSided && !transparentPass && gbufferDoubleSidedPipeline.pipeline() != VK_NULL_HANDLE)
                {
                    drawLayout = gbufferDoubleSidedPipeline.layout();
                    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        gbufferDoubleSidedPipeline.pipeline());
                }
                else if (doubleSided && transparentPass && transparentDoubleSidedPipeline.pipeline() != VK_NULL_HANDLE)
                {
                    drawLayout = transparentDoubleSidedPipeline.layout();
                    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        transparentDoubleSidedPipeline.pipeline());
                }
                else
                {
                    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        transparentPass ? transparentPipeline.pipeline() : gbufferPipeline.pipeline());
                }
                const VkDescriptorSet material = sceneResources.materialDescriptor(instance.materialId);
                if (material != VK_NULL_HANDLE)
                    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        drawLayout, 0, 1, &material, 0, nullptr);
            }
            const glm::mat4 model = glm::make_mat4(instance.transform.data());
            if (shadowPass)
            {
                struct CsmConstants { glm::mat4 lightViewProjection; glm::mat4 model; } constants{
                    cascadeMatrices[std::min<std::uint32_t>(cascade, 3u)], model};
                vkCmdPushConstants(frame.commandBuffer, drawLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                    sizeof(constants), &constants);
            }
            else
            {
                const std::size_t instanceIndex = packet.instances.data() != nullptr
                    ? static_cast<std::size_t>(instancePointer - packet.instances.data())
                    : 0u;
                const bool hasPrevious = previousPacketValid && instanceIndex < previousInstances.size() &&
                    previousInstances[instanceIndex].meshId == instance.meshId &&
                    previousInstances[instanceIndex].materialId == instance.materialId;
                const glm::mat4 previousModel = hasPrevious
                    ? glm::make_mat4(previousInstances[instanceIndex].transform.data())
                    : model;
                const glm::mat4 previousVp = previousPacketValid ? previousViewProjection
                                                                  : packet.camera.viewProjection;
                if (transparentPass)
                {
                    M3TransparentPushConstants constants{};
                    constants.viewProjection = packet.camera.viewProjection;
                    constants.cameraPosition = glm::vec4{
                        glm::vec3(packet.camera.positionAndNear), 1.0f};
                    constants.model = model;
                    if (!packet.lights.empty())
                    {
                        const auto& light = packet.lights.front();
                        constants.lightPositionOrDirection = glm::vec4{
                            light.directionAndType[3] > 0.5f
                                ? glm::vec3{light.directionAndType[0], light.directionAndType[1],
                                      light.directionAndType[2]}
                                : glm::vec3{light.positionAndRadius[0], light.positionAndRadius[1],
                                      light.positionAndRadius[2]},
                            light.directionAndType[3]};
                        constants.lightColorIntensity = glm::vec4{
                            light.colorAndIntensity[0], light.colorAndIntensity[1],
                            light.colorAndIntensity[2], light.colorAndIntensity[3]};
                        constants.lightParameters = glm::vec4{light.positionAndRadius[3],
                            light.spotParams[0], light.spotParams[1], 0.04f};
                    }
                    vkCmdPushConstants(frame.commandBuffer, drawLayout,
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                        sizeof(constants), &constants);
                }
                else
                {
                    M3PushConstants constants{};
                    constants.viewProjection = packet.camera.viewProjection;
                    constants.previousViewProjection = previousVp;
                    constants.model = model;
                    constants.previousModel = previousModel;
                    vkCmdPushConstants(frame.commandBuffer, drawLayout,
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                        sizeof(constants), &constants);
                }
            }
            vkCmdDrawIndexed(frame.commandBuffer, mesh->indexCount, 1, 0, 0, 0);
        }
    };

    // Acquire returns an image in PRESENT_SRC_KHR after the previous frame.
    // Explicitly transition it before the tonemap scope; relying on the
    // dynamic-rendering begin operation is invalid Vulkan and crashes on
    // stricter drivers.
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

    auto addDepthPass = [&](const char* name, std::uint32_t cascade)
    {
        graph.addPass<Graph::FrameGraph::Empty>(name,
            [&](Graph::FrameGraph::Builder& builder, Graph::FrameGraph::Empty&)
            {
                shadow = builder.write(shadow, Graph::ResourceUsage::DepthAttachment);
                Graph::FrameGraphRenderPass::Descriptor descriptor{};
                descriptor.attachments.depth = shadow;
                descriptor.viewport.width = csmResolution;
                descriptor.viewport.height = csmResolution;
                descriptor.layerCount = 1;
                descriptor.clearFlags = Graph::FrameGraphAttachmentFlags::Depth;
                builder.declareRenderPass(name, descriptor);
                builder.sideEffect();
            },
            [&, cascade](const Graph::FrameGraphResources& resources,
                          const Graph::FrameGraph::Empty&, Graph::CommandContext&)
            {
                const auto info = resources.getRenderPassInfo(0);
                const auto* target = static_cast<const VulkanFrameGraphRenderTarget*>(info.target.token);
                const VkImageView view = target != nullptr
                                              ? frameGraphProvider.layerView(
                                                    target->resources[Graph::FrameGraphRenderPass::MAX_COLOR_ATTACHMENTS], cascade)
                                              : VK_NULL_HANDLE;
                 if (view == VK_NULL_HANDLE) return;
                 const VkImage shadowImage = frameGraphProvider.image(
                     target->resources[Graph::FrameGraphRenderPass::MAX_COLOR_ATTACHMENTS]);
                 transitionImage(shadowImage, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_ASPECT_DEPTH_BIT, cascade, 1);
                VkRenderingAttachmentInfo depthAttachment{};
                depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                depthAttachment.imageView = view;
                 depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                depthAttachment.clearValue.depthStencil.depth = 0.0f;
                VkRenderingInfo rendering{};
                rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                rendering.renderArea.extent = {csmResolution, csmResolution};
                rendering.layerCount = 1;
                rendering.pDepthAttachment = &depthAttachment;
                vkCmdBeginRendering(frame.commandBuffer, &rendering);
                vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    csmDepthPipeline.pipeline());
                VkViewport viewport{0.0f, 0.0f, static_cast<float>(csmResolution),
                    static_cast<float>(csmResolution), 0.0f, 1.0f};
                VkRect2D scissor{{0, 0}, {csmResolution, csmResolution}};
                 vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
                 vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
                 drawScene(csmDepthPipeline.layout(), true, false, cascade);
                 vkCmdEndRendering(frame.commandBuffer);
                 if (cascade == 3)
                 {
                     transitionImage(shadowImage, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                         VK_IMAGE_ASPECT_DEPTH_BIT, 0, 4);
                 }
            });
    };
    addDepthPass("CSM shadows 0", 0);
    addDepthPass("CSM shadows 1", 1);
    addDepthPass("CSM shadows 2", 2);
    addDepthPass("CSM shadows 3", 3);

    graph.addPass<Graph::FrameGraph::Empty>("G-buffer",
        [&](Graph::FrameGraph::Builder& builder, Graph::FrameGraph::Empty&)
        {
            gbuffer0 = builder.write(gbuffer0, Graph::ResourceUsage::ColorAttachment);
            gbuffer1 = builder.write(gbuffer1, Graph::ResourceUsage::ColorAttachment);
            gbuffer2 = builder.write(gbuffer2, Graph::ResourceUsage::ColorAttachment);
            motion = builder.write(motion, Graph::ResourceUsage::ColorAttachment);
            depth = builder.write(depth, Graph::ResourceUsage::DepthAttachment);
            Graph::FrameGraphRenderPass::Descriptor descriptor{};
            descriptor.attachments.color[0] = gbuffer0;
            descriptor.attachments.color[1] = gbuffer1;
            descriptor.attachments.color[2] = gbuffer2;
            descriptor.attachments.color[3] = motion;
            descriptor.attachments.depth = depth;
            descriptor.viewport.width = width;
            descriptor.viewport.height = height;
            descriptor.clearFlags = Graph::FrameGraphAttachmentFlags::AllColors |
                                    Graph::FrameGraphAttachmentFlags::Depth;
            builder.declareRenderPass("G-buffer", descriptor);
            builder.sideEffect();
        },
        [&, gbuffer0, gbuffer1, gbuffer2, motion, depth](const Graph::FrameGraphResources& resources,
            const Graph::FrameGraph::Empty&, Graph::CommandContext&)
        {
            const auto info = resources.getRenderPassInfo(0);
             const auto* target = static_cast<const VulkanFrameGraphRenderTarget*>(info.target.token);
             if (target == nullptr) return;
              transitionImage(frameGraphProvider.image(target->resources[0]), VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
             transitionImage(frameGraphProvider.image(target->resources[1]), VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
             transitionImage(frameGraphProvider.image(target->resources[2]), VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
             transitionImage(frameGraphProvider.image(target->resources[3]), VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
             transitionImage(frameGraphProvider.image(target->resources[Graph::FrameGraphRenderPass::MAX_COLOR_ATTACHMENTS]),
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_ASPECT_DEPTH_BIT);
            std::array<VkRenderingAttachmentInfo, 4> colors{};
            for (std::size_t i = 0; i < colors.size(); ++i)
            {
                colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                colors[i].imageView = target->views[i];
                  colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                colors[i].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
            }
            VkRenderingAttachmentInfo depthAttachment{};
            depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depthAttachment.imageView = target->depthView;
                 depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depthAttachment.clearValue.depthStencil.depth = 0.0f;
            VkRenderingInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering.renderArea.extent = swapchainExtent;
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = static_cast<std::uint32_t>(colors.size());
            rendering.pColorAttachments = colors.data();
            rendering.pDepthAttachment = &depthAttachment;
            vkCmdBeginRendering(frame.commandBuffer, &rendering);
            vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                gbufferPipeline.pipeline());
            VkViewport viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
            VkRect2D scissor{{0, 0}, swapchainExtent};
             vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
             vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
             drawScene(gbufferPipeline.layout(), false, false);
             vkCmdEndRendering(frame.commandBuffer);
        });

    if (config.enableClusteredLighting)
    {
        const auto clusterRangesInput = clusterRanges;
        const auto clusterIndicesInput = clusterIndices;
        const auto clusterOverflowInput = clusterOverflow;
        const auto lightBufferInput = lightBuffer;
        const auto clusterCameraInput = clusterCamera;
        graph.addPass<Graph::FrameGraph::Empty>("Cluster Build",
        [&](Graph::FrameGraph::Builder& builder, Graph::FrameGraph::Empty&)
        {
            clusterRanges = builder.write(clusterRanges, Graph::ResourceUsage::Storage);
            clusterIndices = builder.write(clusterIndices, Graph::ResourceUsage::Storage);
            clusterOverflow = builder.write(clusterOverflow, Graph::ResourceUsage::Storage);
            lightBuffer = builder.write(lightBuffer, Graph::ResourceUsage::Storage);
            builder.read(clusterCamera, Graph::ResourceUsage::Uniform);
            builder.sideEffect();
        },
        [&, clusterRangesInput, clusterIndicesInput, clusterOverflowInput, lightBufferInput,
            clusterCameraInput](
            const Graph::FrameGraphResources& resources, const Graph::FrameGraph::Empty&, Graph::CommandContext&)
        {
            vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                clusterBuildPipeline.computePipeline());
            const VkDescriptorSet descriptor = allocateSet(m3ClusterLayout);
            if (descriptor != VK_NULL_HANDLE)
            {
                const auto& ranges = resources.get<Graph::FrameGraphBuffer>(clusterRangesInput);
                const auto& indices = resources.get<Graph::FrameGraphBuffer>(clusterIndicesInput);
                const auto& overflow = resources.get<Graph::FrameGraphBuffer>(clusterOverflowInput);
                const auto& lights = resources.get<Graph::FrameGraphBuffer>(lightBufferInput);
                const auto& cameraBuffer = resources.get<Graph::FrameGraphBuffer>(clusterCameraInput);
                writeStorageBuffer(descriptor, 0, frameGraphProvider.buffer(ranges.native), ranges.descriptor.size);
                writeStorageBuffer(descriptor, 1, frameGraphProvider.buffer(indices.native), indices.descriptor.size);
                writeStorageBuffer(descriptor, 2, frameGraphProvider.buffer(overflow.native), overflow.descriptor.size);
                writeStorageBuffer(descriptor, 3, frameGraphProvider.buffer(lights.native), lights.descriptor.size);
                writeUniformBuffer(descriptor, 4, frameGraphProvider.buffer(cameraBuffer.native),
                    cameraBuffer.descriptor.size);
                if (!packet.lights.empty())
                {
                    const auto* allocation = frameGraphProvider.nativeBufferAllocation(lights.native);
                    if (allocation != nullptr)
                    {
                        const auto bytes = std::span<const std::byte>{
                            reinterpret_cast<const std::byte*>(packet.lights.data()),
                            packet.lights.size_bytes()};
                        (void)gpuAllocator.writeBuffer(*allocation, bytes);
                    }
                }
                struct alignas(16) ClusterCameraData
                {
                    glm::mat4 view;
                    glm::mat4 inverseProjection;
                } cameraData{packet.camera.view, glm::inverse(packet.camera.projection)};
                if (const auto* allocation =
                        frameGraphProvider.nativeBufferAllocation(cameraBuffer.native);
                    allocation != nullptr)
                {
                    const auto bytes = std::span<const std::byte>{
                        reinterpret_cast<const std::byte*>(&cameraData), sizeof(cameraData)};
                    (void)gpuAllocator.writeBuffer(*allocation, bytes);
                }
                vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    clusterBuildPipeline.layout(), 0, 1, &descriptor, 0, nullptr);
            }
            struct ClusterConstants
            {
                std::uint32_t clusterCount, lightCount, maxLightsPerCluster, tilesX, tilesY, slicesZ;
                float nearPlane, farPlane;
            } constants{tileCount, static_cast<std::uint32_t>(packet.lights.size()),
                VulkanM3FrameResources::MaxLightsPerCluster,
                m3FrameResources.tilesX(), m3FrameResources.tilesY(),
                VulkanM3FrameResources::ClusterSlices,
                std::max(1.0e-4f, packet.camera.positionAndNear.w),
                packet.camera.forwardAndFar.w > packet.camera.positionAndNear.w
                    ? packet.camera.forwardAndFar.w : 1000.0f};
            const VkBuffer overflowBuffer = frameGraphProvider.buffer(
                resources.get<Graph::FrameGraphBuffer>(clusterOverflowInput).native);
            vkCmdFillBuffer(frame.commandBuffer, overflowBuffer, 0, VK_WHOLE_SIZE, 0u);
            VkBufferMemoryBarrier2 overflowBarrier{};
            overflowBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            overflowBarrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            overflowBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            overflowBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            overflowBarrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                             VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            overflowBarrier.buffer = overflowBuffer;
            overflowBarrier.size = VK_WHOLE_SIZE;
            VkDependencyInfo overflowDependency{};
            overflowDependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            overflowDependency.bufferMemoryBarrierCount = 1;
            overflowDependency.pBufferMemoryBarriers = &overflowBarrier;
            vkCmdPipelineBarrier2(frame.commandBuffer, &overflowDependency);
             vkCmdPushConstants(frame.commandBuffer, clusterBuildPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                 0, sizeof(constants), &constants);
             vkCmdDispatch(frame.commandBuffer,
                 std::max(1u, (tileCount + VulkanM3FrameResources::ClusterBuildGroupSize - 1u) /
                     VulkanM3FrameResources::ClusterBuildGroupSize), 1, 1);
             if (currentFrame < clusterOverflowReadbacks.size())
             {
                 const VkBuffer readback = clusterOverflowReadbacks[currentFrame].buffer;
                 VkBufferMemoryBarrier2 toCopy{};
                 toCopy.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                 toCopy.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                 toCopy.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                 toCopy.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                 toCopy.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                 toCopy.buffer = overflowBuffer;
                 toCopy.size = sizeof(std::uint32_t);
                 VkDependencyInfo copyDependency{};
                 copyDependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                 copyDependency.bufferMemoryBarrierCount = 1;
                 copyDependency.pBufferMemoryBarriers = &toCopy;
                 vkCmdPipelineBarrier2(frame.commandBuffer, &copyDependency);
                 VkBufferCopy copy{0, 0, sizeof(std::uint32_t)};
                 vkCmdCopyBuffer(frame.commandBuffer, overflowBuffer, readback, 1, &copy);
                 VkBufferMemoryBarrier2 toHost{};
                 toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                 toHost.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                 toHost.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                 toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
                 toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
                 toHost.buffer = readback;
                 toHost.size = sizeof(std::uint32_t);
                 copyDependency.pBufferMemoryBarriers = &toHost;
                 vkCmdPipelineBarrier2(frame.commandBuffer, &copyDependency);
             }
             std::array<VkBufferMemoryBarrier2, 3> clusterReadBarriers{};
            const std::array<VkBuffer, 3> clusterBuffers = {
                frameGraphProvider.buffer(resources.get<Graph::FrameGraphBuffer>(clusterRangesInput).native),
                frameGraphProvider.buffer(resources.get<Graph::FrameGraphBuffer>(clusterIndicesInput).native),
                frameGraphProvider.buffer(resources.get<Graph::FrameGraphBuffer>(clusterOverflowInput).native)};
            for (std::size_t i = 0; i < clusterReadBarriers.size(); ++i)
            {
                 clusterReadBarriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                 clusterReadBarriers[i].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                 clusterReadBarriers[i].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                clusterReadBarriers[i].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                clusterReadBarriers[i].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
                 clusterReadBarriers[i].buffer = clusterBuffers[i];
                 clusterReadBarriers[i].size = VK_WHOLE_SIZE;
                 if (i == 2 && currentFrame < clusterOverflowReadbacks.size())
                 {
                     clusterReadBarriers[i].srcStageMask |= VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                     clusterReadBarriers[i].srcAccessMask |= VK_ACCESS_2_TRANSFER_READ_BIT;
                 }
            }
            VkDependencyInfo clusterDependency{};
            clusterDependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            clusterDependency.bufferMemoryBarrierCount = static_cast<std::uint32_t>(clusterReadBarriers.size());
            clusterDependency.pBufferMemoryBarriers = clusterReadBarriers.data();
            vkCmdPipelineBarrier2(frame.commandBuffer, &clusterDependency);
            });
    }

    const auto deferredGbuffer0 = gbuffer0;
    const auto deferredGbuffer1 = gbuffer1;
    const auto deferredGbuffer2 = gbuffer2;
    const auto deferredDepth = depth;
    const auto deferredShadow = shadow;
    const auto deferredIrradiance = irradiance;
    const auto deferredPrefiltered = prefiltered;
    const auto deferredBrdf = brdfLut;
    const auto deferredClusterRanges = clusterRanges;
    const auto deferredClusterIndices = clusterIndices;
    const auto deferredLightBuffer = lightBuffer;
    const auto deferredShadowConstants = shadowConstantsBuffer;
    graph.addPass<Graph::FrameGraph::Empty>("Clustered deferred lighting",
        [&](Graph::FrameGraph::Builder& builder, Graph::FrameGraph::Empty&)
        {
            builder.read(gbuffer0, Graph::ResourceUsage::Sampled);
            builder.read(gbuffer1, Graph::ResourceUsage::Sampled);
            builder.read(gbuffer2, Graph::ResourceUsage::Sampled);
            builder.read(depth, Graph::ResourceUsage::Sampled);
            builder.read(shadow, Graph::ResourceUsage::Sampled);
            builder.read(irradiance, Graph::ResourceUsage::Sampled);
            builder.read(prefiltered, Graph::ResourceUsage::Sampled);
            builder.read(brdfLut, Graph::ResourceUsage::Sampled);
            builder.read(clusterRanges, Graph::ResourceUsage::Storage);
            builder.read(clusterIndices, Graph::ResourceUsage::Storage);
            builder.read(lightBuffer, Graph::ResourceUsage::Storage);
            builder.read(shadowConstantsBuffer, Graph::ResourceUsage::Uniform);
            hdr = builder.write(hdr, Graph::ResourceUsage::ColorAttachment);
            Graph::FrameGraphRenderPass::Descriptor descriptor{};
            descriptor.attachments.color[0] = hdr;
            descriptor.viewport.width = width;
            descriptor.viewport.height = height;
            descriptor.clearFlags = Graph::FrameGraphAttachmentFlags::Color0;
            builder.declareRenderPass("Clustered deferred lighting", descriptor);
            builder.sideEffect();
        },
        [&, hdr, deferredGbuffer0, deferredGbuffer1, deferredGbuffer2, deferredDepth, deferredShadow,
            deferredIrradiance, deferredPrefiltered, deferredBrdf, deferredClusterRanges,
            deferredClusterIndices, deferredLightBuffer, deferredShadowConstants](
            const Graph::FrameGraphResources& resources, const Graph::FrameGraph::Empty&,
            Graph::CommandContext&)
        {
            const auto info = resources.getRenderPassInfo(0);
            const auto* target = static_cast<const VulkanFrameGraphRenderTarget*>(info.target.token);
            if (target == nullptr) return;
             transitionImage(frameGraphProvider.image(target->resources[0]),
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            transitionImage(frameGraphProvider.image(resources.getTexture(deferredGbuffer0).native),
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            transitionImage(frameGraphProvider.image(resources.getTexture(deferredGbuffer1).native),
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            transitionImage(frameGraphProvider.image(resources.getTexture(deferredGbuffer2).native),
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            transitionImage(frameGraphProvider.image(resources.getTexture(deferredDepth).native),
                VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_IMAGE_ASPECT_DEPTH_BIT);
            if (!iblInitialized)
            {
                transitionImage(frameGraphProvider.image(resources.getTexture(deferredIrradiance).native),
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, 0, 6);
                transitionImage(frameGraphProvider.image(resources.getTexture(deferredPrefiltered).native),
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, 0, 6);
                transitionImage(frameGraphProvider.image(resources.getTexture(deferredBrdf).native),
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                static const ProceduralIblData proceduralIbl = createProceduralIbl();
                const VoidResult irradianceUpload = recordImageUpload(frame.commandBuffer,
                    frameGraphProvider.image(resources.getTexture(deferredIrradiance).native),
                    proceduralIbl.irradiance, proceduralIbl.irradianceCopies);
                const VoidResult prefilteredUpload = recordImageUpload(frame.commandBuffer,
                    frameGraphProvider.image(resources.getTexture(deferredPrefiltered).native),
                    proceduralIbl.prefiltered, proceduralIbl.prefilteredCopies);
                const VoidResult brdfUpload = recordImageUpload(frame.commandBuffer,
                    frameGraphProvider.image(resources.getTexture(deferredBrdf).native),
                    proceduralIbl.brdf, proceduralIbl.brdfCopies);
                if (!irradianceUpload || !prefilteredUpload || !brdfUpload)
                {
                    const auto& error = !irradianceUpload ? irradianceUpload.error()
                        : !prefilteredUpload ? prefilteredUpload.error()
                                             : brdfUpload.error();
                    setError(error.describe());
                    fatalError = true;
                    return;
                }
                transitionImage(frameGraphProvider.image(resources.getTexture(deferredIrradiance).native),
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, 0, 6);
                transitionImage(frameGraphProvider.image(resources.getTexture(deferredPrefiltered).native),
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, 0, 6);
                transitionImage(frameGraphProvider.image(resources.getTexture(deferredBrdf).native),
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                iblInitialized = true;
            }
            VkRenderingAttachmentInfo color{};
            color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            color.imageView = target->views[0];
             color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            color.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
            VkRenderingInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering.renderArea.extent = swapchainExtent;
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachments = &color;
            vkCmdBeginRendering(frame.commandBuffer, &rendering);
            vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                deferredLightingPipeline.pipeline());
            const VkDescriptorSet descriptor = allocateSet(m3LightingLayout);
            if (descriptor != VK_NULL_HANDLE)
            {
                writeSampled(descriptor, 0, frameGraphProvider.view(resources.getTexture(deferredGbuffer0).native));
                writeSampled(descriptor, 1, frameGraphProvider.view(resources.getTexture(deferredGbuffer1).native));
                writeSampled(descriptor, 2, frameGraphProvider.view(resources.getTexture(deferredGbuffer2).native));
                writeSampled(descriptor, 3, frameGraphProvider.view(resources.getTexture(deferredDepth).native));
                writeSampled(descriptor, 4, frameGraphProvider.view(resources.getTexture(deferredShadow).native));
                writeSampled(descriptor, 5, frameGraphProvider.view(resources.getTexture(deferredIrradiance).native));
                 writeSampled(descriptor, 6, frameGraphProvider.view(resources.getTexture(deferredPrefiltered).native));
                 writeSampled(descriptor, 7, frameGraphProvider.view(resources.getTexture(deferredBrdf).native));
                const auto& ranges = resources.get<Graph::FrameGraphBuffer>(deferredClusterRanges);
                const auto& indices = resources.get<Graph::FrameGraphBuffer>(deferredClusterIndices);
                const auto& lights = resources.get<Graph::FrameGraphBuffer>(deferredLightBuffer);
                const auto& shadowData = resources.get<Graph::FrameGraphBuffer>(deferredShadowConstants);
                if (!packet.lights.empty())
                {
                    if (const auto* allocation =
                            frameGraphProvider.nativeBufferAllocation(lights.native);
                        allocation != nullptr)
                    {
                        const auto bytes = std::span<const std::byte>{
                            reinterpret_cast<const std::byte*>(packet.lights.data()),
                            packet.lights.size_bytes()};
                        (void)gpuAllocator.writeBuffer(*allocation, bytes);
                    }
                }
                writeStorageBuffer(descriptor, 20, frameGraphProvider.buffer(ranges.native), ranges.descriptor.size);
                writeStorageBuffer(descriptor, 21, frameGraphProvider.buffer(indices.native), indices.descriptor.size);
                writeStorageBuffer(descriptor, 22, frameGraphProvider.buffer(lights.native), lights.descriptor.size);
                writeUniformBuffer(descriptor, 23, frameGraphProvider.buffer(shadowData.native),
                    shadowData.descriptor.size);
                struct alignas(16) ShadowData
                {
                    std::array<glm::mat4, 4> lightViewProjection;
                    glm::vec4 splits;
                    glm::vec4 params;
                } shadowUpload{cascadeMatrices, cascadeSplits,
                    glm::vec4{1.0f / static_cast<float>(csmResolution), 0.00035f, 0.0015f, 0.0f}};
                if (const auto* allocation =
                        frameGraphProvider.nativeBufferAllocation(shadowData.native);
                    allocation != nullptr)
                {
                    const auto bytes = std::span<const std::byte>{
                        reinterpret_cast<const std::byte*>(&shadowUpload), sizeof(shadowUpload)};
                    (void)gpuAllocator.writeBuffer(*allocation, bytes);
                }
                writeSampler(descriptor);
                vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    deferredLightingPipeline.layout(), 0, 1, &descriptor, 0, nullptr);
            }
            VkViewport viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
            VkRect2D scissor{{0, 0}, swapchainExtent};
            struct DeferredConstants
            {
                glm::mat4 inverseViewProjection;
                glm::vec4 cameraPosition;
                glm::vec4 viewportAndInvViewport;
                glm::uvec4 clusterParams;
                glm::vec4 depthParams;
            } constants{};
            constants.inverseViewProjection = packet.camera.inverseViewProjection;
            constants.cameraPosition = packet.camera.positionAndNear;
            constants.viewportAndInvViewport = packet.camera.viewportAndInvViewport;
            constants.clusterParams = glm::uvec4{
                m3FrameResources.tilesX(),
                m3FrameResources.tilesY(),
                VulkanM3FrameResources::ClusterSlices,
                static_cast<std::uint32_t>(packet.lights.size())};
            constants.depthParams = glm::vec4{
                std::max(1.0e-4f, packet.camera.positionAndNear.w),
                packet.camera.forwardAndFar.w > packet.camera.positionAndNear.w
                    ? packet.camera.forwardAndFar.w : 1000.0f,
                config.enableClusteredLighting ? 1.0f : 0.0f, 0.0f};
            vkCmdPushConstants(frame.commandBuffer, deferredLightingPipeline.layout(),
                VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);
            vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
            vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
            vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
            vkCmdEndRendering(frame.commandBuffer);
        });

    if (config.enableTransparency)
    {
        graph.addPass<Graph::FrameGraph::Empty>("Forward transparency",
            [&](Graph::FrameGraph::Builder& builder, Graph::FrameGraph::Empty&)
            {
                builder.read(depth, Graph::ResourceUsage::DepthAttachment);
                hdr = builder.write(hdr, Graph::ResourceUsage::ColorAttachment);
                Graph::FrameGraphRenderPass::Descriptor descriptor{};
                descriptor.attachments.color[0] = hdr;
                descriptor.attachments.depth = depth;
                descriptor.viewport.width = width;
                descriptor.viewport.height = height;
                descriptor.clearFlags = Graph::FrameGraphAttachmentFlags::None;
                builder.declareRenderPass("Forward transparency", descriptor);
                builder.sideEffect();
            },
            [&, hdr](const Graph::FrameGraphResources& resources, const Graph::FrameGraph::Empty&,
                Graph::CommandContext&)
            {
                const auto info = resources.getRenderPassInfo(0);
                const auto* target = static_cast<const VulkanFrameGraphRenderTarget*>(info.target.token);
                if (target == nullptr) return;
                transitionImage(frameGraphProvider.image(resources.getTexture(depth).native),
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT);
                VkRenderingAttachmentInfo color{};
                color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                color.imageView = target->views[0];
                 color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                VkRenderingAttachmentInfo depthAttachment{};
                depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                depthAttachment.imageView = target->depthView;
                 depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
                VkRenderingInfo rendering{};
                rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                rendering.renderArea.extent = swapchainExtent;
                rendering.layerCount = 1;
                rendering.colorAttachmentCount = 1;
                rendering.pColorAttachments = &color;
                rendering.pDepthAttachment = &depthAttachment;
                vkCmdBeginRendering(frame.commandBuffer, &rendering);
                vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    transparentPipeline.pipeline());
                drawScene(transparentPipeline.layout(), false, true);
                vkCmdEndRendering(frame.commandBuffer);
            });
    }

    auto& historyRead = taaHistoryFlip ? historyB : historyA;
    auto& historyWrite = taaHistoryFlip ? historyA : historyB;
    const bool frameHistoryFlip = taaHistoryFlip;
    const auto taaHdr = hdr;
    const auto taaMotion = motion;
    const auto taaHistoryRead = historyRead;
    graph.addPass<Graph::FrameGraph::Empty>("TAA resolve",
        [&](Graph::FrameGraph::Builder& builder, Graph::FrameGraph::Empty&)
        {
            builder.read(hdr, Graph::ResourceUsage::Sampled);
            builder.read(motion, Graph::ResourceUsage::Sampled);
            builder.read(historyRead, Graph::ResourceUsage::Sampled);
            historyWrite = builder.write(historyWrite, Graph::ResourceUsage::Storage);
            builder.sideEffect();
        },
        [&, taaHdr, taaMotion, taaHistoryRead](const Graph::FrameGraphResources& resources,
            const Graph::FrameGraph::Empty&, Graph::CommandContext&)
        {
            transitionImage(frameGraphProvider.image(resources.getTexture(taaHdr).native),
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            transitionImage(frameGraphProvider.image(resources.getTexture(taaMotion).native),
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            const VkImage historyReadImage = frameGraphProvider.image(resources.getTexture(taaHistoryRead).native);
            const VkImage historyWriteImage = frameGraphProvider.image(resources.getTexture(historyWrite).native);
             const bool readInitialized = frameHistoryFlip ? taaHistoryInitializedB : taaHistoryInitializedA;
             const bool writeInitialized = frameHistoryFlip ? taaHistoryInitializedA : taaHistoryInitializedB;
            transitionImage(historyReadImage,
                readInitialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            transitionImage(historyWriteImage,
                writeInitialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            {
                vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    taaPipeline.computePipeline());
                const VkDescriptorSet descriptor = allocateSet(m3TaaLayout);
                if (descriptor != VK_NULL_HANDLE)
                {
                    writeSampled(descriptor, 0, frameGraphProvider.view(resources.getTexture(taaHdr).native));
                    writeSampled(descriptor, 1, frameGraphProvider.view(resources.getTexture(taaHistoryRead).native));
                    writeSampled(descriptor, 2, frameGraphProvider.view(resources.getTexture(taaMotion).native));
                    writeSampler(descriptor);
                    writeStorage(descriptor, 20, frameGraphProvider.view(resources.getTexture(historyWrite).native));
                    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                        taaPipeline.layout(), 0, 1, &descriptor, 0, nullptr);
                }
                struct TaaConstants { std::uint32_t extent[2]; float historyWeight; float sharpen; } constants{{width, height},
                    (config.enableTaa && taaHistoryValid) ? 0.9f : 0.0f,
                    config.enableTaa ? 0.05f : 0.0f};
                vkCmdPushConstants(frame.commandBuffer, taaPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                    0, sizeof(constants), &constants);
                vkCmdDispatch(frame.commandBuffer, (width + 7u) / 8u, (height + 7u) / 8u, 1);
                transitionImage(historyWriteImage, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                if (frameHistoryFlip) taaHistoryInitializedA = true;
                else taaHistoryInitializedB = true;
            }
        });

    const auto tonemapInput = historyWrite;
    graph.addPass<Graph::FrameGraph::Empty>("ACES tonemap",
        [&](Graph::FrameGraph::Builder& builder, Graph::FrameGraph::Empty&)
        {
            builder.read(tonemapInput, Graph::ResourceUsage::Sampled);
            output = builder.write(output, Graph::ResourceUsage::ColorAttachment);
            Graph::FrameGraphRenderPass::Descriptor descriptor{};
            descriptor.attachments.color[0] = output;
            descriptor.viewport.width = width;
            descriptor.viewport.height = height;
            descriptor.clearFlags = Graph::FrameGraphAttachmentFlags::Color0;
            builder.declareRenderPass("ACES tonemap", descriptor);
            builder.sideEffect();
        },
        [&, tonemapInput](const Graph::FrameGraphResources& resources, const Graph::FrameGraph::Empty&, Graph::CommandContext&)
        {
            VkRenderingAttachmentInfo color{};
            color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            color.imageView = importedTarget.view;
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            color.clearValue.color = {{0.018f, 0.028f, 0.055f, 1.0f}};
            VkRenderingInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering.renderArea.extent = swapchainExtent;
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachments = &color;
            vkCmdBeginRendering(frame.commandBuffer, &rendering);
            vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                tonemapPipeline.pipeline());
            const VkDescriptorSet descriptor = allocateSet(m3TonemapLayout);
            if (descriptor != VK_NULL_HANDLE)
            {
                writeSampled(descriptor, 0, frameGraphProvider.view(resources.getTexture(tonemapInput).native));
                writeSampler(descriptor);
                vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    tonemapPipeline.layout(), 0, 1, &descriptor, 0, nullptr);
            }
            VkViewport viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
            VkRect2D scissor{{0, 0}, swapchainExtent};
            vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
            vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
            struct TonemapConstants { float exposure; float padding[3]; } tonemapConstants{
                config.exposure, {0.0f, 0.0f, 0.0f}};
            vkCmdPushConstants(frame.commandBuffer, tonemapPipeline.layout(),
                VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(tonemapConstants), &tonemapConstants);
            vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
            vkCmdEndRendering(frame.commandBuffer);
        });
    graph.present(output);

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
        return fail(lastError.empty() ? "Vulkan M3 pass recording failed" : lastError,
            Halcyon::ErrorCode::Backend);
    }
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
    presentBarrier.subresourceRange.levelCount = 1;
    presentBarrier.subresourceRange.layerCount = 1;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &presentBarrier;
    vkCmdPipelineBarrier2(frame.commandBuffer, &dependency);
    if (timestampsEnabled)
    {
        vkCmdWriteTimestamp2(frame.commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
            frameContext.timestampPool, frame.queryBase + 1);
    }
    taaHistoryFlip = !frameHistoryFlip;
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
        result = impl_->createM3Descriptors();
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
        result = impl_->sceneResources.initialize(impl_->device,
            impl_->physicalDevice,
            impl_->frames.front().commandPool,
            impl_->graphicsQueue,
            impl_->gpuAllocator,
            impl_->gpuUploader);
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
    impl_->deviceMemoryBytes = impl_->gpuAllocator.allocatedBytes();
    return result;
}

void Renderer::invalidateTaaHistory() noexcept
{
    if (impl_ != nullptr)
    {
        impl_->taaHistoryValid = false;
        impl_->hasRenderedFrame = false;
        impl_->previousPacketValid = false;
        impl_->previousInstances.clear();
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
