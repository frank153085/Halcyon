#pragma once

#include "Core/Result.h"
#include "GpuResourceManager.h"
#include "../Scene/StaticSceneLoader.h"

#include <cstdint>
#include <string>
#include <vulkan/vulkan.h>
#include <vector>

namespace Halcyon::Vulkan
{

// Owns the small set of resources used by the learning/demo application.  It
// deliberately sits below Renderer so the renderer only coordinates frame
// flow and does not know how a texture descriptor or upload buffer is built.
class VulkanDemoResources final
{
public:
    struct SceneDraw
    {
        std::uint32_t firstIndex = 0;
        std::uint32_t indexCount = 0;
        std::int32_t vertexOffset = 0;
        std::uint32_t materialIndex = 0;
        std::uint32_t textureIndex = 0;
    };

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
        const char* startupMeshPath,
        const char* startupScenePath = nullptr);
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
    [[nodiscard]] std::uint32_t primitiveCount() const noexcept { return primitiveCount_; }
    [[nodiscard]] const std::vector<SceneDraw>& sceneDraws() const noexcept
    {
        return sceneDraws_;
    }
    [[nodiscard]] const std::vector<VkDescriptorSet>& sceneTextureDescriptorSets() const noexcept
    {
        return sceneTextureDescriptorSets_;
    }

private:
    [[nodiscard]] Halcyon::Result<void> createTriangleGeometry();
    void loadStartupResources(const char* texturePath, const char* meshPath) noexcept;
    [[nodiscard]] Halcyon::Result<void> loadStaticScene(const char* scenePath) noexcept;
    void destroyDescriptorResources() noexcept;
    [[nodiscard]] bool createTextureDescriptors(std::vector<TextureResource>& textures) noexcept;

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
    std::uint32_t primitiveCount_ = 0;
    std::vector<TextureResource> sceneTextures_;
    std::vector<VkDescriptorSet> sceneTextureDescriptorSets_;
    std::vector<SceneDraw> sceneDraws_;
};

} // namespace Halcyon::Vulkan
