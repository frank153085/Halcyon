#pragma once

#include "GpuAllocator.h"
#include "GpuUploader.h"

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
};

struct MeshVertex
{
    float position[3]{};
    float normal[3]{};
    float uv[2]{};
};

struct MeshResource
{
    BufferAllocation vertexBuffer{};
    BufferAllocation indexBuffer{};
    std::uint32_t indexCount = 0;
};

class GpuResourceManager final
{
public:
    GpuResourceManager() noexcept = default;
    GpuResourceManager(const GpuResourceManager&) = delete;
    GpuResourceManager& operator=(const GpuResourceManager&) = delete;

    void initialize(VkDevice device,
        VkCommandPool commandPool,
        VkQueue queue,
        GpuAllocator& allocator,
        GpuUploader& uploader) noexcept;
    [[nodiscard]] Halcyon::Result<TextureResource> loadTexture2D(const std::string& path);
    [[nodiscard]] Halcyon::Result<MeshResource> loadObj(const std::string& path);
    void destroy(TextureResource& texture) noexcept;
    void destroy(MeshResource& mesh) noexcept;
    void shutdown() noexcept;

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    GpuAllocator* allocator_ = nullptr;
    GpuUploader* uploader_ = nullptr;
};

} // namespace Halcyon::Vulkan
