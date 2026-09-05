#pragma once

#include "../Scene/SceneDatabase.h"
#include "GpuAllocator.h"
#include "GpuUploader.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Halcyon::Vulkan
{

struct TextureResource
{
    ImageAllocation allocation{};
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkExtent3D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::uint32_t mipLevels = 1;
};

struct MeshVertex
{
    float position[3]{};
    float normal[3]{};
    float uv[2]{};
    float tangent[4]{1.0f, 0.0f, 0.0f, 1.0f};
};

struct MeshResource
{
    BufferAllocation vertexBuffer{};
    BufferAllocation indexBuffer{};
    std::uint32_t indexCount = 0;
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
};

class GpuResourceManager final
{
public:
    GpuResourceManager() noexcept = default;
    GpuResourceManager(const GpuResourceManager&) = delete;
    GpuResourceManager& operator=(const GpuResourceManager&) = delete;

    void initialize(VkDevice device,
        VkPhysicalDevice physicalDevice,
        VkCommandPool commandPool,
        VkQueue queue,
        GpuAllocator& allocator,
        GpuUploader& uploader) noexcept;
    // Color textures default to sRGB; linear data (normal/roughness/AO) can
    // explicitly pass false when the full material uploader is enabled.
    [[nodiscard]] Halcyon::Result<TextureResource> loadTexture2D(
        const std::string& path, bool srgb = true);
    // Create a deterministic 1x1 default texture without touching the
    // filesystem.  M3 materials use white for base color/AO, a flat normal,
    // and black for metallic/emissive channels when an image is absent.
    [[nodiscard]] Halcyon::Result<TextureResource> loadSolidColorTexture(
        std::array<std::uint8_t, 4> rgba, bool srgb = false);
    [[nodiscard]] Halcyon::Result<MeshResource> uploadMesh(
        const Halcyon::Renderer::Scene::SceneMesh& mesh);
    void destroy(TextureResource& texture) noexcept;
    void destroy(MeshResource& mesh) noexcept;
    void shutdown() noexcept;

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    GpuAllocator* allocator_ = nullptr;
    GpuUploader* uploader_ = nullptr;
};

} // namespace Halcyon::Vulkan
