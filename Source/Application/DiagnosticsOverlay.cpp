#include "DiagnosticsOverlay.h"

#include "Engine/EngineInternal.h"
#include "WindowInternal.h"

#ifndef HALCYON_ENABLE_IMGUI
#define HALCYON_ENABLE_IMGUI 0
#endif

#if HALCYON_ENABLE_IMGUI
#include "Renderer/Vulkan/HalcyonVulkanRenderer.h"

#ifndef GLFW_INCLUDE_VULKAN
#define GLFW_INCLUDE_VULKAN
#endif
#include <GLFW/glfw3.h>
#include <algorithm>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>
#endif

#include <new>

namespace Halcyon::ApplicationInternal
{

struct DiagnosticsOverlay::Impl
{
#if HALCYON_ENABLE_IMGUI
    Engine* engine = nullptr;
    bool initialized = false;
    FrameStats stats{};
    std::uint64_t frameIndex = 0;
    Capabilities capabilities{};
    static DiagnosticsOverlay* active;
#endif
};

#if HALCYON_ENABLE_IMGUI
DiagnosticsOverlay* DiagnosticsOverlay::Impl::active = nullptr;

void renderOverlay(VkCommandBuffer commandBuffer) noexcept
{
    auto* overlay = DiagnosticsOverlay::Impl::active;
    if (overlay == nullptr || ImGui::GetDrawData() == nullptr)
    {
        return;
    }
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
}
#endif

DiagnosticsOverlay::~DiagnosticsOverlay()
{
    shutdown();
    delete impl_;
    impl_ = nullptr;
}

Result<void> DiagnosticsOverlay::initialize(Platform::Window& window, Engine& engine) noexcept
{
#if !HALCYON_ENABLE_IMGUI
    (void)window;
    (void)engine;
    return Result<void>::success();
#else
    shutdown();
    delete impl_;
    impl_ = nullptr;
    impl_ = new (std::nothrow) Impl{};
    if (impl_ == nullptr)
    {
        return Result<void>::failure(MakeError(
            ErrorCode::OutOfMemory, "diagnostics state allocation failed", "Diagnostics"));
    }
    auto* renderer = Internal::EngineAccess::renderer(engine);
    GLFWwindow* nativeWindow = Platform::Internal::WindowAccess::nativeHandle(window);
    if (renderer == nullptr || nativeWindow == nullptr || renderer->instance() == VK_NULL_HANDLE ||
        renderer->physicalDevice() == VK_NULL_HANDLE || renderer->device() == VK_NULL_HANDLE ||
        renderer->graphicsQueue() == VK_NULL_HANDLE)
    {
        return Result<void>::failure(
            MakeError(ErrorCode::InvalidState, "Vulkan renderer is unavailable", "Diagnostics"));
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    if (!ImGui_ImplGlfw_InitForVulkan(nativeWindow, true))
    {
        ImGui::DestroyContext();
        return Result<void>::failure(MakeError(
            ErrorCode::Backend, "ImGui GLFW backend initialization failed", "Diagnostics"));
    }

    const std::uint32_t imageCount = std::max(2u, renderer->swapchainImageCount());
    VkFormat colorFormat = renderer->swapchainFormat();
    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.ApiVersion = VK_API_VERSION_1_3;
    initInfo.Instance = renderer->instance();
    initInfo.PhysicalDevice = renderer->physicalDevice();
    initInfo.Device = renderer->device();
    initInfo.QueueFamily = renderer->capabilities().graphicsQueueFamily;
    initInfo.Queue = renderer->graphicsQueue();
    initInfo.DescriptorPoolSize = 1000;
    initInfo.MinImageCount = imageCount;
    initInfo.ImageCount = imageCount;
    initInfo.UseDynamicRendering = true;
    initInfo.PipelineInfoMain.RenderPass = VK_NULL_HANDLE;
    initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &colorFormat;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.depthAttachmentFormat =
        renderer->depthFormat();
    if (!ImGui_ImplVulkan_Init(&initInfo))
    {
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        return Result<void>::failure(MakeError(
            ErrorCode::Backend, "ImGui Vulkan backend initialization failed", "Diagnostics"));
    }

    impl_->engine = &engine;
    impl_->initialized = true;
    Impl::active = this;
    renderer->setOverlayCallback(renderOverlay);
    return Result<void>::success();
#endif
}

void DiagnosticsOverlay::beginFrame(
    const FrameStats& stats, std::uint64_t frameIndex, const Capabilities& capabilities) noexcept
{
#if HALCYON_ENABLE_IMGUI
    if (impl_ == nullptr || !impl_->initialized)
    {
        return;
    }
    impl_->stats = stats;
    impl_->frameIndex = frameIndex;
    impl_->capabilities = capabilities;
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGui::Begin("Halcyon Diagnostics");
    ImGui::Text("Frame: %llu", static_cast<unsigned long long>(frameIndex));
    ImGui::Text("GPU: %s", capabilities.deviceName.c_str());
    ImGui::Text("Dynamic rendering: %s | Sync2: %s | Timeline: %s",
        capabilities.dynamicRendering ? "yes" : "no",
        capabilities.synchronization2 ? "yes" : "no",
        capabilities.timelineSemaphore ? "yes" : "no");
    ImGui::Text("CPU: %.3f ms", stats.cpuFrameMs);
    if (stats.gpuFrameMs >= 0.0)
    {
        ImGui::Text("GPU: %.3f ms", stats.gpuFrameMs);
    }
    else
    {
        ImGui::TextUnformatted("GPU: waiting for timestamp");
    }
    ImGui::Text(
        "Device memory: %llu bytes", static_cast<unsigned long long>(stats.deviceMemoryBytes));
    ImGui::Text("Rendered: %s", stats.rendered ? "yes" : "no");
    for (const auto& pass : stats.gpuPasses)
    {
        ImGui::Text("Pass %s: %.3f ms", pass.name.c_str(), pass.gpuFrameMs);
    }
    ImGui::End();
#else
    (void)stats;
    (void)frameIndex;
    (void)capabilities;
#endif
}

void DiagnosticsOverlay::endFrame() noexcept
{
#if HALCYON_ENABLE_IMGUI
    if (impl_ != nullptr && impl_->initialized)
    {
        ImGui::Render();
    }
#endif
}

void DiagnosticsOverlay::shutdown() noexcept
{
#if HALCYON_ENABLE_IMGUI
    if (impl_ == nullptr || !impl_->initialized)
    {
        return;
    }
    auto* renderer =
        impl_->engine != nullptr ? Internal::EngineAccess::renderer(*impl_->engine) : nullptr;
    if (renderer != nullptr)
    {
        renderer->setOverlayCallback(nullptr);
        if (renderer->device() != VK_NULL_HANDLE)
        {
            (void)vkDeviceWaitIdle(renderer->device());
        }
    }
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    Impl::active = nullptr;
    impl_->engine = nullptr;
    impl_->initialized = false;
#endif
}

} // namespace Halcyon::ApplicationInternal
