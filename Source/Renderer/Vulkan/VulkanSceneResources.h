#pragma once

#include "../Scene/SceneDatabase.h"
#include "Core/Result.h"
#include "GpuResourceManager.h"

#include <cstdint>
#include <array>
#include <string>
#include <unordered_map>
#include <vulkan/vulkan.h>
#include <vector>

namespace Halcyon::Vulkan
{

// GPU companion for the backend-neutral SceneDatabase. Stable CPU handles are
// remapped to dense indices while SceneManager builds a frame packet; file
// parsing and scene/entity policy stay in SceneManager.
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
        VkPhysicalDevice physicalDevice,
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
    // Resolve a stable SceneDatabase slot to a dense GPU index. Frame packets
    // submitted to render() are already remapped and never dereference a
    // backend allocation through Handle::index().
    [[nodiscard]] std::uint32_t meshDenseIndex(std::uint32_t stableIndex) const noexcept;
    [[nodiscard]] std::uint32_t materialDenseIndex(std::uint32_t stableIndex) const noexcept;
    [[nodiscard]] std::uint32_t textureDenseIndex(std::uint32_t stableIndex) const noexcept;
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
        std::uint32_t normalTexture = 0;
        std::uint32_t metallicRoughnessTexture = 0;
        std::uint32_t emissiveTexture = 0;
        std::uint32_t occlusionTexture = 0;
        BufferAllocation factorsBuffer{};
    };

    // std140-compatible material constants consumed by gbuffer and forward
    // transparency shaders. Every member is a vec4 so the ABI is identical
    // across HLSL, GLSL/SPIR-V and the host upload path.
    struct alignas(16) MaterialGpuData
    {
        std::array<float, 4> baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
        std::array<float, 4> emissiveFactor{0.0f, 0.0f, 0.0f, 0.0f};
        std::array<float, 4> factors{0.0f, 1.0f, 0.5f, 0.0f};
    };
    static_assert(sizeof(MaterialGpuData) == 48);

    [[nodiscard]] Halcyon::Result<void> createDescriptorLayout();
    [[nodiscard]] Halcyon::Result<void> rebuildMaterialDescriptors();
    [[nodiscard]] Halcyon::Result<BufferAllocation> createMaterialBuffer(
        const Halcyon::Renderer::Scene::SceneMaterial& material);
    [[nodiscard]] Halcyon::Result<std::string> retainTexture(
        const Halcyon::Renderer::Scene::SceneTexture& texture);
    void releaseTexture(std::uint32_t index) noexcept;
    [[nodiscard]] const TextureResource* texture(std::uint32_t index) const noexcept;
    void destroyDescriptorPool() noexcept;

    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
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
    std::unordered_map<std::uint32_t, std::uint32_t> meshDenseByStable_;
    std::unordered_map<std::uint32_t, std::uint32_t> materialDenseByStable_;
    std::unordered_map<std::uint32_t, std::uint32_t> textureDenseByStable_;
    std::vector<std::uint32_t> denseMeshStable_;
    std::vector<std::uint32_t> denseMaterialStable_;
    std::vector<std::uint32_t> denseTextureStable_;
    std::vector<std::uint32_t> freeMeshDense_;
    std::vector<std::uint32_t> freeMaterialDense_;
    std::vector<std::uint32_t> freeTextureDense_;
    std::unordered_map<std::string, SharedTexture> sharedTextures_;
    std::unordered_map<std::uint32_t, std::string> textureKeys_;
};

} // namespace Halcyon::Vulkan
