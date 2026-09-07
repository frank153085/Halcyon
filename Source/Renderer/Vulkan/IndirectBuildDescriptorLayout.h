#pragma once

#include "VulkanPipeline.h"

#include <array>
#include <cstdint>
#include <vulkan/vulkan.h>

namespace Halcyon::Vulkan
{

// Single source of truth for build_indirect_commands.comp descriptor ABI.
// Layout creation, shader reflection checks, and descriptor writes all derive
// from this table so adding a storage buffer is one edit.
inline constexpr std::uint32_t kIndirectBuildBindingCount = 10;

enum class IndirectBuildBinding : std::uint32_t
{
    VisibleIndices = 0,
    MeshMaterials = 1,
    IndirectCommands = 2,
    IndirectCount = 3,
    MeshDraws = 4,
    MeshHeads = 5,
    MeshNext = 6,
    GroupedVisible = 7,
    GroupedCount = 8,
    VisibleCount = 9,
};

inline constexpr VkShaderStageFlags kIndirectBuildStage = VK_SHADER_STAGE_COMPUTE_BIT;

[[nodiscard]] constexpr VkDescriptorSetLayoutBinding makeIndirectStorageBinding(
    std::uint32_t binding) noexcept
{
    return VkDescriptorSetLayoutBinding{
        binding,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        1,
        kIndirectBuildStage,
        nullptr};
}

inline constexpr std::array<VkDescriptorSetLayoutBinding, kIndirectBuildBindingCount>
    kIndirectBuildLayoutBindings = {
        makeIndirectStorageBinding(0),
        makeIndirectStorageBinding(1),
        makeIndirectStorageBinding(2),
        makeIndirectStorageBinding(3),
        makeIndirectStorageBinding(4),
        makeIndirectStorageBinding(5),
        makeIndirectStorageBinding(6),
        makeIndirectStorageBinding(7),
        makeIndirectStorageBinding(8),
        makeIndirectStorageBinding(9),
};

[[nodiscard]] constexpr std::array<DescriptorBindingDesc, kIndirectBuildBindingCount>
indirectBuildAbi() noexcept
{
    std::array<DescriptorBindingDesc, kIndirectBuildBindingCount> abi{};
    for (std::uint32_t i = 0; i < kIndirectBuildBindingCount; ++i)
    {
        abi[i] = DescriptorBindingDesc{0, kIndirectBuildLayoutBindings[i]};
    }
    return abi;
}

enum class IndirectBuildPass : std::uint8_t
{
    Main,
    Phase1,
    Phase2,
    Shadow,
};

struct IndirectBuildBuffers
{
    VkBuffer visibleIndices = VK_NULL_HANDLE;
    VkDeviceSize visibleIndicesSize = 0;
    VkBuffer meshMaterials = VK_NULL_HANDLE;
    VkDeviceSize meshMaterialsSize = 0;
    VkBuffer indirectCommands = VK_NULL_HANDLE;
    VkDeviceSize indirectCommandsSize = 0;
    VkBuffer indirectCount = VK_NULL_HANDLE;
    VkBuffer meshDraws = VK_NULL_HANDLE;
    VkDeviceSize meshDrawsSize = 0;
    VkBuffer meshHeads = VK_NULL_HANDLE;
    VkDeviceSize meshHeadsSize = 0;
    VkBuffer meshNext = VK_NULL_HANDLE;
    VkDeviceSize meshNextSize = 0;
    VkBuffer groupedVisible = VK_NULL_HANDLE;
    VkDeviceSize groupedVisibleSize = 0;
    VkBuffer groupedCount = VK_NULL_HANDLE;
    VkBuffer visibleCount = VK_NULL_HANDLE;
};

} // namespace Halcyon::Vulkan
