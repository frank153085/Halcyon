#pragma once

#include "../Scene/SceneDatabase.h"
#include "Core/Result.h"
#include "GpuResourceManager.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vulkan/vulkan.h>

namespace Halcyon::Vulkan
{

// GPU companion for the backend-neutral SceneDatabase. Stable CPU resource
// handles are represented by their slot index in the immutable FramePacket.
// File parsing and scene/entity policy stay in SceneManager.
class VulkanSceneResources final
{
public:
    VulkanSceneResources() noexcept = default;
    ~VulkanSceneResources() noexcept
    {
        cleanup();
    }
    VulkanSceneResources(const VulkanSceneResources&) = delete;
    VulkanSceneResources& operator=(const VulkanSceneResources&) = delete;

    [[nodiscard]] Halcyon::Result<void> initialize(VkDevice device,
        VkCommandPool uploadCommandPool,
        VkQueue graphicsQueue,
        GpuAllocator& allocator,
        GpuUploader& uploader);
    [[nodiscard]] Halcyon::Result<void> uploadAsset(
        const Halcyon::Renderer::Scene::SceneDatabase& database,
        const Halcyon::Renderer::Scene::SceneImportResult& imported);
    [[nodiscard]] Halcyon::Result<void> releaseAsset(
        const Halcyon::Renderer::Scene::SceneImportResult& imported);
    void cleanup() noexcept;

    [[nodiscard]] const MeshResource* mesh(std::uint32_t index) const noexcept;
    [[nodiscard]] VkDescriptorSet materialDescriptor(std::uint32_t index) const noexcept;
    [[nodiscard]] VkDescriptorSetLayout textureSetLayout() const noexcept
    {
        return textureSetLayout_;
    }
    [[nodiscard]] bool textured() const noexcept
    {
        return textureSetLayout_ != VK_NULL_HANDLE;
    }
    [[nodiscard]] std::uint32_t primitiveCount() const noexcept
    {
        return static_cast<std::uint32_t>(meshes_.size());
    }

private:
    struct SharedTexture
    {
        TextureResource resource{};
        std::uint32_t references = 0;
    };

    struct MaterialResource
    {
        std::uint32_t baseColorTexture = 0;
    };

    [[nodiscard]] Halcyon::Result<void> createDescriptorLayout();
    [[nodiscard]] Halcyon::Result<void> rebuildMaterialDescriptors();
    [[nodiscard]] Halcyon::Result<std::string> retainTexture(
        const Halcyon::Renderer::Scene::SceneTexture& texture);
    void releaseTexture(std::uint32_t index) noexcept;
    [[nodiscard]] const TextureResource* texture(std::uint32_t index) const noexcept;
    void destroyDescriptorPool() noexcept;

    VkDevice device_ = VK_NULL_HANDLE;
    VkCommandPool uploadCommandPool_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    GpuAllocator* allocator_ = nullptr;
    GpuUploader* uploader_ = nullptr;
    GpuResourceManager resourceManager_;
    VkDescriptorSetLayout textureSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool textureDescriptorPool_ = VK_NULL_HANDLE;
    std::unordered_map<std::uint32_t, MeshResource> meshes_;
    std::unordered_map<std::uint32_t, MaterialResource> materials_;
    std::unordered_map<std::uint32_t, VkDescriptorSet> materialDescriptors_;
    std::unordered_map<std::string, SharedTexture> sharedTextures_;
    std::unordered_map<std::uint32_t, std::string> textureKeys_;
};

} // namespace Halcyon::Vulkan
