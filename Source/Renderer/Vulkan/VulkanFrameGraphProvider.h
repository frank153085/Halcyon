#pragma once

#include "GpuAllocator.h"
#include "Renderer/Graph/FrameGraphRenderPass.h"
#include "Renderer/Graph/FrameGraphTypes.h"

#include <memory>
#include <array>
#include <string>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.h>

namespace Halcyon::Vulkan
{

// Native owner for one FrameGraph resource. The token address is stable for
// the lifetime of the allocation (tokens are heap allocated rather than
// stored inline in an unordered_map).
struct VulkanFrameGraphResource
{
    VkDevice device = VK_NULL_HANDLE;
    GpuAllocator* allocator = nullptr;
    BufferAllocation buffer{};
    ImageAllocation image{};
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent3D extent{};
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    std::uint32_t mipLevels = 1;
    std::uint32_t arrayLayers = 1;
    std::vector<VkImageView> layerViews;
    bool isBuffer = false;
    bool persistent = false;
    bool imported = false;
};

struct VulkanFrameGraphRenderTarget
{
    VkDevice device = VK_NULL_HANDLE;
    std::array<Halcyon::Renderer::Graph::FrameGraphNativeResource,
        Halcyon::Renderer::Graph::FrameGraphRenderPass::ATTACHMENT_COUNT> resources{};
    std::array<VkImageView, Halcyon::Renderer::Graph::FrameGraphRenderPass::ATTACHMENT_COUNT> views{};
    std::uint32_t colorCount = 0;
    VkImageView depthView = VK_NULL_HANDLE;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
};

class VulkanFrameGraphProvider final : public Halcyon::Renderer::Graph::FrameGraphResourceProvider
{
public:
    VulkanFrameGraphProvider() noexcept = default;
    ~VulkanFrameGraphProvider() noexcept override { shutdown(); }

    VulkanFrameGraphProvider(const VulkanFrameGraphProvider&) = delete;
    VulkanFrameGraphProvider& operator=(const VulkanFrameGraphProvider&) = delete;

    [[nodiscard]] Halcyon::Result<void> initialize(
        VkDevice device, VkPhysicalDevice physicalDevice, GpuAllocator& allocator);

    bool create(const Halcyon::Renderer::Graph::FrameGraphResourceCreateInfo&, 
        Halcyon::Renderer::Graph::FrameGraphNativeResource&) noexcept override;
    void destroy(const Halcyon::Renderer::Graph::FrameGraphNativeResource&) noexcept override;
    bool createRenderTarget(const Halcyon::Renderer::Graph::FrameGraphRenderTargetCreateInfo&,
        Halcyon::Renderer::Graph::FrameGraphNativeResource&) noexcept override;
    void destroyRenderTarget(const Halcyon::Renderer::Graph::FrameGraphNativeResource&) noexcept override;

    [[nodiscard]] VkImage image(Halcyon::Renderer::Graph::FrameGraphNativeResource token) const noexcept;
    [[nodiscard]] VkBuffer buffer(Halcyon::Renderer::Graph::FrameGraphNativeResource token) const noexcept;
    [[nodiscard]] VkImageView view(Halcyon::Renderer::Graph::FrameGraphNativeResource token) const noexcept;
    [[nodiscard]] VkImage nativeImage(Halcyon::Renderer::Graph::FrameGraphNativeResource token) const noexcept
    {
        return image(token);
    }
    [[nodiscard]] VkImageView nativeView(Halcyon::Renderer::Graph::FrameGraphNativeResource token) const noexcept
    {
        return view(token);
    }
    [[nodiscard]] const ImageAllocation* nativeAllocation(
        Halcyon::Renderer::Graph::FrameGraphNativeResource token) const noexcept;
    [[nodiscard]] const BufferAllocation* nativeBufferAllocation(
        Halcyon::Renderer::Graph::FrameGraphNativeResource token) const noexcept;
    [[nodiscard]] VkImageView layerView(Halcyon::Renderer::Graph::FrameGraphNativeResource token,
        std::uint32_t layer) const noexcept;

    // Transient resources are released by FrameGraph at their last use. This
    // method is for resize/shutdown and releases cached persistent objects.
    // Destruction is deferred until the command buffer that references a
    // resource has completed. The renderer assigns a monotonically increasing
    // serial before recording each frame and collects completed serials after
    // waiting on the corresponding frame fence.
    void beginFrame(std::uint64_t serial) noexcept { activeSerial_ = serial; }
    void collectCompleted(std::uint64_t serial) noexcept;
    void flushDeferred() noexcept;
    void resetTransient() noexcept;
    void recreatePersistent() noexcept;
    void shutdown() noexcept;

private:
    [[nodiscard]] static VkFormat toVkFormat(Halcyon::Renderer::Graph::TextureFormat format) noexcept;
    [[nodiscard]] static VkImageUsageFlags imageUsage(Halcyon::Renderer::Graph::ResourceUsage usage) noexcept;
    [[nodiscard]] static VkBufferUsageFlags bufferUsage(Halcyon::Renderer::Graph::ResourceUsage usage) noexcept;
    [[nodiscard]] bool createImage(const Halcyon::Renderer::Graph::FrameGraphResourceCreateInfo&,
        VulkanFrameGraphResource&) noexcept;
    [[nodiscard]] bool createBuffer(const Halcyon::Renderer::Graph::FrameGraphResourceCreateInfo&, 
        VulkanFrameGraphResource&) noexcept;
    void release(VulkanFrameGraphResource&) noexcept;

    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    GpuAllocator* allocator_ = nullptr;
    std::vector<std::unique_ptr<VulkanFrameGraphResource>> resources_;
    std::unordered_map<std::string, VulkanFrameGraphResource*> persistent_;
    std::vector<std::unique_ptr<VulkanFrameGraphRenderTarget>> renderTargets_;
    struct DeferredResource
    {
        VulkanFrameGraphResource* resource = nullptr;
        std::uint64_t serial = 0;
    };
    std::vector<DeferredResource> deferred_;
    std::uint64_t activeSerial_ = 0;
};

} // namespace Halcyon::Vulkan
