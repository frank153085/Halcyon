#pragma once

#include "../Scene/SceneDatabase.h"
#include "../Scene/VirtualGeometry.h"
#include "../Scene/GpuScene.h"
#include "Core/Result.h"
#include "GpuResourceManager.h"

#include <cstdint>
#include <array>
#include <string>
#include <unordered_map>
#include <vulkan/vulkan.h>
#include <vector>
#include <glm/glm.hpp>

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
        GpuUploader& uploader,
        bool enableGpuDrivenMeshes);
    [[nodiscard]] Halcyon::Result<void> uploadAsset(
        const Halcyon::Renderer::Scene::SceneDatabase& database,
        const Halcyon::Renderer::Scene::SceneImportResult& imported);
    [[nodiscard]] Halcyon::Result<void> releaseAsset(
        const Halcyon::Renderer::Scene::SceneImportResult& imported);
    void cleanup() noexcept;

    [[nodiscard]] const MeshResource* mesh(std::uint32_t index) const noexcept;
    [[nodiscard]] VkBuffer gpuDrivenVertexBuffer() const noexcept
    {
        return gpuDrivenVertices_.buffer;
    }
    [[nodiscard]] VkBuffer gpuDrivenIndexBuffer() const noexcept
    {
        return gpuDrivenIndices_.buffer;
    }
    [[nodiscard]] VkBuffer meshDrawBuffer() const noexcept
    {
        return meshDraws_.buffer;
    }
    [[nodiscard]] std::uint32_t meshDrawCount() const noexcept
    {
        return static_cast<std::uint32_t>(meshDrawRows_.size());
    }
    [[nodiscard]] VkDescriptorSet materialDescriptor(std::uint32_t index) const noexcept;
    [[nodiscard]] std::uint32_t materialCount() const noexcept
    {
        return static_cast<std::uint32_t>(denseMaterialStable_.size());
    }
    [[nodiscard]] std::uint32_t textureCount() const noexcept
    {
        return static_cast<std::uint32_t>(denseTextureStable_.size());
    }
    [[nodiscard]] const Halcyon::Renderer::Scene::VirtualGeometryAsset* virtualGeometry(
        std::uint32_t meshIndex) const noexcept
    {
        const auto found = virtualGeometryByMesh_.find(meshIndex);
        return found == virtualGeometryByMesh_.end() ? nullptr : found->second.get();
    }
    [[nodiscard]] const Halcyon::Renderer::Scene::VirtualGeometryAsset* virtualGeometryDense(
        std::uint32_t denseIndex) const noexcept
    {
        if (denseIndex >= denseMeshStable_.size())
            return nullptr;
        return virtualGeometry(denseMeshStable_[denseIndex]);
    }
    struct VirtualGeometryGpuBuffers
    {
        BufferAllocation vertices{};
        BufferAllocation indices{};
        BufferAllocation meshletVertices{};
        BufferAllocation meshletTriangles{};
        BufferAllocation meshlets{};
        BufferAllocation lods{};
        BufferAllocation clusters{};
        BufferAllocation dagNodes{};
        BufferAllocation dagEdges{};
        BufferAllocation lodStates{};
        BufferAllocation boundaryVertices{};
        BufferAllocation clusterMeshletIndices{};
        BufferAllocation meshletDagNodes{};
        BufferAllocation clusterAdjacencyOffsets{};
        BufferAllocation clusterAdjacencyIndices{};
    };
    struct alignas(16) VirtualGeometryGpuMeshlet
    {
        std::uint32_t vertexOffset = 0;
        std::uint32_t vertexCount = 0;
        std::uint32_t triangleOffset = 0;
        std::uint32_t triangleCount = 0;
        std::uint32_t indexOffset = 0;
        std::uint32_t indexCount = 0;
        std::uint32_t primitiveIndex = 0;
        std::uint32_t lodIndex = 0;
        glm::vec4 sphere{0.0f};
        glm::vec4 cone{0.0f};
        float geometricError = 0.0f;
        std::array<float, 3> padding{};
    };
    static_assert(sizeof(VirtualGeometryGpuMeshlet) == 80);
    struct alignas(16) VirtualGeometryGpuCluster
    {
        std::uint32_t meshletOffset = 0, meshletCount = 0;
        std::uint32_t vertexOffset = 0, vertexCount = 0;
        std::uint32_t triangleCount = 0, lodDepth = 0;
        std::uint32_t primitiveIndex = 0;
        glm::vec4 sphere{0.0f};
        float geometricError = 0.0f;
        std::array<float, 3> padding{};
    };
    struct alignas(16) VirtualGeometryGpuDagNode
    {
        std::uint32_t clusterIndex = 0, parentIndex = 0;
        std::uint32_t firstChild = 0, childCount = 0;
        std::uint32_t lodDepth = 0, flags = 0;
        glm::vec4 sphere{0.0f};
        float geometricError = 0.0f;
        std::array<float, 3> padding{};
    };
    static_assert(sizeof(VirtualGeometryGpuCluster) == 64);
    static_assert(sizeof(VirtualGeometryGpuDagNode) == 64);
    [[nodiscard]] const VirtualGeometryGpuBuffers* virtualGeometryBuffers(
        std::uint32_t meshIndex) const noexcept
    {
        const auto found = virtualGeometryGpuByMesh_.find(meshIndex);
        return found == virtualGeometryGpuByMesh_.end() ? nullptr : &found->second;
    }
    [[nodiscard]] const VirtualGeometryGpuBuffers* virtualGeometryBuffersDense(
        std::uint32_t denseIndex) const noexcept
    {
        if (denseIndex >= denseMeshStable_.size())
            return nullptr;
        return virtualGeometryBuffers(denseMeshStable_[denseIndex]);
    }
    [[nodiscard]] Halcyon::Renderer::Scene::MaterialGpuData materialRow(
        std::uint32_t denseIndex) const noexcept;
    [[nodiscard]] bool virtualGeometryMaterialCompatible(
        std::uint32_t denseIndex) const noexcept;
    [[nodiscard]] const TextureResource* textureDense(
        std::uint32_t denseIndex) const noexcept;
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
        Halcyon::Renderer::Scene::MaterialGpuData bindlessRow{};
        bool virtualGeometryCompatible = false;
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
    [[nodiscard]] Halcyon::Result<void> rebuildGpuDrivenMeshes();
    [[nodiscard]] Halcyon::Result<BufferAllocation> createMaterialBuffer(
        const Halcyon::Renderer::Scene::SceneMaterial& material);
    [[nodiscard]] Halcyon::Result<std::string> retainTexture(
        const Halcyon::Renderer::Scene::SceneTexture& texture);
    [[nodiscard]] Halcyon::Result<VirtualGeometryGpuBuffers> uploadVirtualGeometry(
        const Halcyon::Renderer::Scene::VirtualGeometryAsset& asset);
    void destroyVirtualGeometry(VirtualGeometryGpuBuffers& buffers) noexcept;
    void releaseTexture(std::uint32_t index) noexcept;
    [[nodiscard]] const TextureResource* texture(std::uint32_t index) const noexcept;
    void destroyDescriptorPool() noexcept;

    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkCommandPool uploadCommandPool_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    GpuAllocator* allocator_ = nullptr;
    GpuUploader* uploader_ = nullptr;
    bool gpuDrivenMeshesEnabled_ = false;
    GpuResourceManager resourceManager_;
    VkDescriptorSetLayout textureSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool textureDescriptorPool_ = VK_NULL_HANDLE;
    std::unordered_map<std::uint32_t, MeshResource> meshes_;
    std::unordered_map<std::uint32_t, std::shared_ptr<const Halcyon::Renderer::Scene::VirtualGeometryAsset>> virtualGeometryByMesh_;
    std::unordered_map<std::uint32_t, VirtualGeometryGpuBuffers> virtualGeometryGpuByMesh_;
    BufferAllocation gpuDrivenVertices_{};
    BufferAllocation gpuDrivenIndices_{};
    BufferAllocation meshDraws_{};
    std::vector<Halcyon::Renderer::Scene::MeshDrawRow> meshDrawRows_;
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
