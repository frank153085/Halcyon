#pragma once

#include "../Resources/BindlessTable.h"
#include "Core/Result.h"

#include <vulkan/vulkan.h>

namespace Halcyon::Vulkan
{

namespace Resources = ::Halcyon::Renderer::Resources;

// Vulkan-facing companion to the backend-neutral BindlessTable.  The CPU
// table owns generation and timeline policy; this class owns the descriptor
// set and translates typed slot writes into Vulkan descriptors.
class VulkanBindlessTable final
{
public:
    VulkanBindlessTable() noexcept = default;
    ~VulkanBindlessTable() noexcept
    {
        shutdown();
    }
    VulkanBindlessTable(const VulkanBindlessTable&) = delete;
    VulkanBindlessTable& operator=(const VulkanBindlessTable&) = delete;

    [[nodiscard]] Halcyon::Result<void> initialize(
        VkDevice device, Resources::BindlessTableConfig config = {});
    void shutdown() noexcept;

    [[nodiscard]] Halcyon::Result<Resources::DescriptorHandle> allocate(
        Resources::DescriptorType type,
        Resources::DescriptorValue value = {},
        std::uint64_t completedTimeline = 0);
    [[nodiscard]] Halcyon::Result<void> release(Resources::DescriptorType type,
        Resources::DescriptorHandle handle,
        std::uint64_t retireTimeline);
    [[nodiscard]] Halcyon::Result<void> touch(Resources::DescriptorType type,
        Resources::DescriptorHandle handle,
        std::uint64_t submittedTimeline);
    [[nodiscard]] std::size_t collect(std::uint64_t completedTimeline);

    [[nodiscard]] Halcyon::Result<void> setDefault(
        Resources::DescriptorType type, Resources::DescriptorValue value);
    [[nodiscard]] Halcyon::Result<void> update(Resources::DescriptorType type,
        Resources::DescriptorHandle handle,
        Resources::DescriptorValue value);

    [[nodiscard]] Halcyon::Result<void> writeImage(Resources::DescriptorType type,
        Resources::DescriptorHandle handle,
        const VkDescriptorImageInfo& image);
    [[nodiscard]] Halcyon::Result<void> writeBuffer(Resources::DescriptorType type,
        Resources::DescriptorHandle handle,
        const VkDescriptorBufferInfo& buffer);

    [[nodiscard]] VkDescriptorSetLayout layout() const noexcept
    {
        return layout_;
    }
    [[nodiscard]] VkDescriptorSet descriptorSet() const noexcept
    {
        return descriptorSet_;
    }
    [[nodiscard]] bool initialized() const noexcept
    {
        return device_ != VK_NULL_HANDLE && descriptorSet_ != VK_NULL_HANDLE;
    }
    [[nodiscard]] Resources::BindlessTable& table() noexcept
    {
        return table_;
    }
    [[nodiscard]] const Resources::BindlessTable& table() const noexcept
    {
        return table_;
    }

private:
    [[nodiscard]] static VkDescriptorType nativeType(Resources::DescriptorType type) noexcept;
    [[nodiscard]] Halcyon::Result<void> validateWrite(
        Resources::DescriptorType type, Resources::DescriptorHandle handle) const;

    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    Resources::BindlessTable table_;
};

} // namespace Halcyon::Vulkan
