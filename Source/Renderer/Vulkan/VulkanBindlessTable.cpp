#include "VulkanBindlessTable.h"

#include <array>
#include <string>

namespace Halcyon::Vulkan
{
namespace
{

using Resources::DescriptorType;

[[nodiscard]] std::uint32_t capacityFor(
    const Resources::BindlessTableConfig& config, DescriptorType type) noexcept
{
    switch (type)
    {
        case DescriptorType::SampledImage:
            return config.sampledImageCapacity;
        case DescriptorType::StorageImage:
            return config.storageImageCapacity;
        case DescriptorType::UniformBuffer:
            return config.uniformBufferCapacity;
        case DescriptorType::StorageBuffer:
            return config.storageBufferCapacity;
        case DescriptorType::Sampler:
            return config.samplerCapacity;
        case DescriptorType::Count:
            break;
    }
    return 0;
}

[[nodiscard]] Halcyon::Result<void> backendFailure(const char* operation, VkResult result)
{
    return Halcyon::Result<void>::failure(Halcyon::Error{Halcyon::ErrorCode::Backend,
        std::string(operation) + " failed (VkResult " + std::to_string(static_cast<int>(result)) +
            ")"});
}

} // namespace

Halcyon::Result<void> VulkanBindlessTable::initialize(
    VkDevice device, Resources::BindlessTableConfig config)
{
    if (device == VK_NULL_HANDLE)
    {
        return Halcyon::Result<void>::failure(
            Halcyon::Error{Halcyon::ErrorCode::InvalidState, "Vulkan device is null"});
    }
    if (initialized())
    {
        return Halcyon::Result<void>::failure(
            Halcyon::Error{Halcyon::ErrorCode::AlreadyExists, "bindless table is initialized"});
    }

    const auto cpuResult = table_.initialize(config);
    if (!cpuResult)
    {
        return cpuResult;
    }
    device_ = device;

    std::array<VkDescriptorSetLayoutBinding, Resources::kDescriptorTypeCount> bindings{};
    std::array<VkDescriptorPoolSize, Resources::kDescriptorTypeCount> poolSizes{};
    for (std::size_t i = 0; i < Resources::kDescriptorTypeCount; ++i)
    {
        const auto type = static_cast<DescriptorType>(i);
        const auto native = nativeType(type);
        const auto count = capacityFor(config, type);
        bindings[i].binding = static_cast<std::uint32_t>(i);
        bindings[i].descriptorType = native;
        bindings[i].descriptorCount = count;
        bindings[i].stageFlags = VK_SHADER_STAGE_ALL;
        poolSizes[i].type = native;
        poolSizes[i].descriptorCount = count;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    VkResult result = vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &layout_);
    if (result != VK_SUCCESS)
    {
        shutdown();
        return backendFailure("vkCreateDescriptorSetLayout", result);
    }

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    result = vkCreateDescriptorPool(device_, &poolInfo, nullptr, &pool_);
    if (result != VK_SUCCESS)
    {
        shutdown();
        return backendFailure("vkCreateDescriptorPool", result);
    }

    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = pool_;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &layout_;
    result = vkAllocateDescriptorSets(device_, &allocateInfo, &descriptorSet_);
    if (result != VK_SUCCESS)
    {
        shutdown();
        return backendFailure("vkAllocateDescriptorSets", result);
    }
    return Halcyon::Result<void>::success();
}

void VulkanBindlessTable::shutdown() noexcept
{
    if (device_ != VK_NULL_HANDLE)
    {
        if (pool_ != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(device_, pool_, nullptr);
        }
        if (layout_ != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(device_, layout_, nullptr);
        }
    }
    descriptorSet_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    table_.shutdown();
}

Halcyon::Result<Resources::DescriptorHandle> VulkanBindlessTable::allocate(
    Resources::DescriptorType type,
    Resources::DescriptorValue value,
    std::uint64_t completedTimeline)
{
    if (!initialized())
    {
        return Halcyon::Result<Resources::DescriptorHandle>::failure(Halcyon::Error{
            Halcyon::ErrorCode::InvalidState, "Vulkan bindless table is not initialized"});
    }
    return table_.allocate(type, value, completedTimeline);
}

Halcyon::Result<void> VulkanBindlessTable::release(Resources::DescriptorType type,
    Resources::DescriptorHandle handle,
    std::uint64_t retireTimeline)
{
    return table_.release(type, handle, retireTimeline);
}

Halcyon::Result<void> VulkanBindlessTable::touch(Resources::DescriptorType type,
    Resources::DescriptorHandle handle,
    std::uint64_t submittedTimeline)
{
    return table_.touch(type, handle, submittedTimeline);
}

std::size_t VulkanBindlessTable::collect(std::uint64_t completedTimeline)
{
    return table_.collect(completedTimeline);
}

Halcyon::Result<void> VulkanBindlessTable::setDefault(
    Resources::DescriptorType type, Resources::DescriptorValue value)
{
    return table_.setDefault(type, value);
}

Halcyon::Result<void> VulkanBindlessTable::update(Resources::DescriptorType type,
    Resources::DescriptorHandle handle,
    Resources::DescriptorValue value)
{
    return table_.update(type, handle, value);
}

VkDescriptorType VulkanBindlessTable::nativeType(Resources::DescriptorType type) noexcept
{
    switch (type)
    {
        case DescriptorType::SampledImage:
            return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case DescriptorType::StorageImage:
            return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        case DescriptorType::UniformBuffer:
            return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case DescriptorType::StorageBuffer:
            return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case DescriptorType::Sampler:
            return VK_DESCRIPTOR_TYPE_SAMPLER;
        case DescriptorType::Count:
            break;
    }
    return VK_DESCRIPTOR_TYPE_MAX_ENUM;
}

Halcyon::Result<void> VulkanBindlessTable::validateWrite(
    Resources::DescriptorType type, Resources::DescriptorHandle handle) const
{
    if (!initialized())
    {
        return Halcyon::Result<void>::failure(Halcyon::Error{
            Halcyon::ErrorCode::InvalidState, "Vulkan bindless table is not initialized"});
    }
    if (!table_.contains(type, handle))
    {
        return Halcyon::Result<void>::failure(
            Halcyon::Error{Halcyon::ErrorCode::NotFound, "descriptor handle is invalid or stale"});
    }
    return Halcyon::Result<void>::success();
}

Halcyon::Result<void> VulkanBindlessTable::writeImage(Resources::DescriptorType type,
    Resources::DescriptorHandle handle,
    const VkDescriptorImageInfo& image)
{
    const auto valid = validateWrite(type, handle);
    if (!valid)
    {
        return valid;
    }
    if (type != DescriptorType::SampledImage && type != DescriptorType::StorageImage &&
        type != DescriptorType::Sampler)
    {
        return Halcyon::Result<void>::failure(Halcyon::Error{
            Halcyon::ErrorCode::InvalidArgument, "descriptor type requires a buffer write"});
    }
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptorSet_;
    write.dstBinding = static_cast<std::uint32_t>(type);
    write.dstArrayElement = handle.index();
    write.descriptorCount = 1;
    write.descriptorType = nativeType(type);
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    return Halcyon::Result<void>::success();
}

Halcyon::Result<void> VulkanBindlessTable::writeBuffer(Resources::DescriptorType type,
    Resources::DescriptorHandle handle,
    const VkDescriptorBufferInfo& buffer)
{
    const auto valid = validateWrite(type, handle);
    if (!valid)
    {
        return valid;
    }
    if (type != DescriptorType::UniformBuffer && type != DescriptorType::StorageBuffer)
    {
        return Halcyon::Result<void>::failure(Halcyon::Error{
            Halcyon::ErrorCode::InvalidArgument, "descriptor type requires an image write"});
    }
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptorSet_;
    write.dstBinding = static_cast<std::uint32_t>(type);
    write.dstArrayElement = handle.index();
    write.descriptorCount = 1;
    write.descriptorType = nativeType(type);
    write.pBufferInfo = &buffer;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    return Halcyon::Result<void>::success();
}

} // namespace Halcyon::Vulkan
