#pragma once

#include "VirtualGeometry.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Halcyon::Renderer::Scene
{

inline constexpr std::uint32_t kVirtualGeometryDefaultPageSize = 128u * 1024u;

struct VirtualGeometryStreamRange
{
    std::uint64_t offset = 0u;
    std::uint64_t size = 0u;
};

struct VirtualGeometryPageDependencyRange
{
    std::uint32_t offset = 0u;
    std::uint32_t count = 0u;
};

// Stable virtual byte-address layout for the pageable GPU geometry streams.
// Meshlet, Cluster, DAG and dependency metadata remain resident and therefore
// are deliberately absent from these four ranges.
struct VirtualGeometryPageLayout
{
    std::uint32_t pageSize = kVirtualGeometryDefaultPageSize;
    std::uint32_t pageCount = 0u;
    std::uint64_t rawSize = 0u;
    VirtualGeometryStreamRange vertices{};
    VirtualGeometryStreamRange meshletVertices{};
    VirtualGeometryStreamRange meshletTriangles{};
    VirtualGeometryStreamRange indices{};
    std::vector<VirtualGeometryPageDependencyRange> nodeDependencies;
    std::vector<std::uint32_t> dependencyPageIndices;
    std::vector<std::uint32_t> rootPageIndices;
};

[[nodiscard]] Halcyon::Result<VirtualGeometryPageLayout>
buildVirtualGeometryPageLayout(const VirtualGeometryAsset& asset,
    std::uint32_t pageSize = kVirtualGeometryDefaultPageSize);

// Materializes exactly one virtual page without building a second full-size
// geometry payload in memory. The returned bytes use the same ABI currently
// uploaded to Vulkan for each source stream.
[[nodiscard]] Halcyon::Result<std::vector<std::byte>>
serializeVirtualGeometryPage(const VirtualGeometryAsset& asset,
    const VirtualGeometryPageLayout& layout, std::uint32_t pageIndex);

[[nodiscard]] bool virtualGeometryNodeDependsOnPage(
    const VirtualGeometryPageLayout& layout, std::uint32_t nodeIndex,
    std::uint32_t pageIndex) noexcept;

} // namespace Halcyon::Renderer::Scene
