#pragma once

#include "Core/Result.h"
#include "GpuResourceManager.h"

#include <cstdint>
#include <vulkan/vulkan.h>

namespace Halcyon::Vulkan
{

// Owns the small set of resources used by the learning/demo application.  It
// deliberately sits below Renderer so the renderer only coordinates frame
// flow and does not know how a texture descriptor or upload buffer is built.
class VulkanDemoResources final
{
public:
    VulkanDemoResources() noexcept = default;
    ~VulkanDemoResources() noexcept
    {
        cleanup();
    }
    VulkanDemoResources(const VulkanDemoResources&) = delete;
    VulkanDemoResources& operator=(const VulkanDemoResources&) = delete;

    [[nodiscard]] Halcyon::Result<void> initialize(VkDevice device,
        VkCommandPool uploadCommandPool,
        VkQueue graphicsQueue,
        GpuAllocator& allocator,
        GpuUploader& uploader,
        const char* startupTexturePath,
        const char* startupMeshPath);
    void cleanup() noexcept;

    [[nodiscard]] Halcyon::Result<TextureResource> loadTexture2D(const char* path);
    [[nodiscard]] Halcyon::Result<MeshResource> loadObj(const char* path);
    void destroy(TextureResource& texture) noexcept;
    void destroy(MeshResource& mesh) noexcept;

    [[nodiscard]] const BufferAllocation& triangleVertexBuffer() const noexcept
    {
        return triangleVertexBuffer_;
    }
    [[nodiscard]] const MeshResource& demoMesh() const noexcept
    {
        return demoMesh_;
    }
    [[nodiscard]] VkDescriptorSetLayout textureSetLayout() const noexcept
    {
        return textureSetLayout_;
    }
    [[nodiscard]] VkDescriptorSet textureDescriptorSet() const noexcept
    {
        return textureDescriptorSet_;
    }
    [[nodiscard]] bool textured() const noexcept
    {
        return textured_;
    }

private:
    [[nodiscard]] Halcyon::Result<void> createTriangleGeometry();
    void loadStartupResources(const char* texturePath, const char* meshPath) noexcept;
    void destroyDescriptorResources() noexcept;

    VkDevice device_ = VK_NULL_HANDLE;
    VkCommandPool uploadCommandPool_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    GpuAllocator* allocator_ = nullptr;
    GpuUploader* uploader_ = nullptr;
    GpuResourceManager resourceManager_;
    BufferAllocation triangleVertexBuffer_{};
    TextureResource demoTexture_{};
    MeshResource demoMesh_{};
    VkDescriptorSetLayout textureSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool textureDescriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet textureDescriptorSet_ = VK_NULL_HANDLE;
    bool textured_ = false;
};

} // namespace Halcyon::Vulkan
