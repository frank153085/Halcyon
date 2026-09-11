#pragma once

#include "VirtualGeometry.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace Halcyon::Renderer::Scene
{

inline constexpr std::uint32_t kVirtualGeometryDefaultPageSize = 128u * 1024u;
inline constexpr std::uint32_t kVirtualGeometryPageHeaderSize = 16u;

enum VirtualGeometryPageRequestReason : std::uint32_t
{
    VirtualGeometryPageRequestVisible = 1u << 0u,
    VirtualGeometryPageRequestDependency = 1u << 1u,
    VirtualGeometryPageRequestPrefetch = 1u << 2u,
};

struct alignas(16) VirtualGeometryPageRequest
{
    std::uint32_t pageIndex = 0u;
    std::uint32_t priorityBits = 0u;
    std::uint32_t reason = VirtualGeometryPageRequestVisible;
    std::uint32_t reserved = 0u;
};

static_assert(sizeof(VirtualGeometryPageRequest) == 16u);
static_assert(alignof(VirtualGeometryPageRequest) == 16u);
static_assert(std::is_standard_layout_v<VirtualGeometryPageRequest>);

struct VirtualGeometryPageUsage
{
    std::uint32_t pageIndex = 0u;
    std::uint32_t generation = 0u;
};

static_assert(sizeof(VirtualGeometryPageUsage) == 8u);
static_assert(std::is_standard_layout_v<VirtualGeometryPageUsage>);

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

struct VirtualGeometryPackedMeshletAddress
{
    std::uint32_t pageIndex = 0u;
    std::uint32_t vertexOffset = 0u;
    std::uint32_t triangleOffset = 0u;
};

struct VirtualGeometryPageMeshlet
{
    std::uint32_t meshletIndex = 0u;
    std::uint32_t vertexOffset = 0u;
    std::uint32_t triangleOffset = 0u;
};

struct VirtualGeometryPageDescriptor
{
    std::uint32_t meshletOffset = 0u;
    std::uint32_t meshletCount = 0u;
    std::uint32_t vertexCount = 0u;
    std::uint32_t meshletVertexCount = 0u;
    std::uint32_t triangleByteCount = 0u;
};

// Cluster/Meshlet-atomic GPU pages. A meshlet is never split across pages.
// Adjacent Clusters may share a page, but each Cluster still occupies a
// consecutive page range and is only considered resident when every dependency
// page is present. The four logical stream ranges describe the reconstructed
// CPU tables and are not a GPU byte-address map.
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
    std::vector<VirtualGeometryPackedMeshletAddress> meshletAddresses;
    std::vector<VirtualGeometryPageDescriptor> pages;
    std::vector<VirtualGeometryPageMeshlet> pageMeshlets;
};

[[nodiscard]] Halcyon::Result<VirtualGeometryPageLayout>
buildVirtualGeometryPageLayout(const VirtualGeometryAsset& asset,
    std::uint32_t pageSize = kVirtualGeometryDefaultPageSize);

// Materializes exactly one virtual page without building a second full-size
// geometry payload in memory. The returned bytes are an atomic Cluster/Meshlet
// payload addressed through the GPU page table.
[[nodiscard]] Halcyon::Result<std::vector<std::byte>>
serializeVirtualGeometryPage(const VirtualGeometryAsset& asset,
    const VirtualGeometryPageLayout& layout, std::uint32_t pageIndex);

[[nodiscard]] Halcyon::Result<void> unpackVirtualGeometryPage(
    VirtualGeometryAsset& asset, const VirtualGeometryPageLayout& layout,
    std::uint32_t pageIndex, std::span<const std::byte> page);

[[nodiscard]] bool virtualGeometryNodeDependsOnPage(
    const VirtualGeometryPageLayout& layout, std::uint32_t nodeIndex,
    std::uint32_t pageIndex) noexcept;

} // namespace Halcyon::Renderer::Scene
