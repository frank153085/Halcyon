#include "VirtualGeometryPages.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

namespace Halcyon::Renderer::Scene
{
namespace
{

template <typename T>
Halcyon::Result<T> pageError(Halcyon::ErrorCode code, std::string message)
{
    return Halcyon::Result<T>::failure(
        {code, std::move(message), "VirtualGeometryPages"});
}

bool checkedByteSize(std::size_t count, std::size_t stride,
    std::uint64_t& result) noexcept
{
    if (stride != 0u && count > std::numeric_limits<std::uint64_t>::max() / stride)
        return false;
    result = static_cast<std::uint64_t>(count) * stride;
    return true;
}

bool alignPage(std::uint64_t value, std::uint32_t pageSize,
    std::uint64_t& result) noexcept
{
    const std::uint64_t mask = pageSize - 1u;
    if (value > std::numeric_limits<std::uint64_t>::max() - mask)
        return false;
    result = (value + mask) & ~mask;
    return true;
}

void appendPageRange(std::vector<std::uint32_t>& pages,
    std::uint64_t byteOffset, std::uint64_t byteSize, std::uint32_t pageSize)
{
    if (byteSize == 0u)
        return;
    const std::uint64_t first = byteOffset / pageSize;
    const std::uint64_t last = (byteOffset + byteSize - 1u) / pageSize;
    for (std::uint64_t page = first; page <= last; ++page)
        pages.push_back(static_cast<std::uint32_t>(page));
}

void copyPageOverlap(std::span<std::byte> page, std::uint64_t pageOffset,
    VirtualGeometryStreamRange range, const void* source)
{
    if (range.size == 0u)
        return;
    const std::uint64_t pageEnd = pageOffset + page.size();
    const std::uint64_t rangeEnd = range.offset + range.size;
    const std::uint64_t begin = std::max(pageOffset, range.offset);
    const std::uint64_t end = std::min(pageEnd, rangeEnd);
    if (begin >= end)
        return;
    const std::size_t destinationOffset = static_cast<std::size_t>(begin - pageOffset);
    const std::size_t sourceOffset = static_cast<std::size_t>(begin - range.offset);
    const std::size_t size = static_cast<std::size_t>(end - begin);
    std::memcpy(page.data() + destinationOffset,
        static_cast<const std::byte*>(source) + sourceOffset, size);
}

} // namespace

Halcyon::Result<VirtualGeometryPageLayout> buildVirtualGeometryPageLayout(
    const VirtualGeometryAsset& asset, std::uint32_t pageSize)
{
    if (pageSize < 4096u || pageSize > 1024u * 1024u ||
        (pageSize & (pageSize - 1u)) != 0u)
        return pageError<VirtualGeometryPageLayout>(Halcyon::ErrorCode::InvalidArgument,
            "page size must be a power of two between 4 KiB and 1 MiB");
    if (asset.vertices.empty() || asset.meshlets.empty() || asset.clusters.empty() ||
        asset.dagNodes.empty())
        return pageError<VirtualGeometryPageLayout>(Halcyon::ErrorCode::InvalidArgument,
            "asset has no pageable virtual geometry");

    VirtualGeometryPageLayout layout;
    layout.pageSize = pageSize;
    std::uint64_t cursor = 0u;
    auto addStream = [&](std::size_t count, std::size_t stride,
                         VirtualGeometryStreamRange& range)
    {
        std::uint64_t size = 0u;
        if (!checkedByteSize(count, stride, size) || !alignPage(cursor, pageSize, cursor) ||
            cursor > std::numeric_limits<std::uint64_t>::max() - size)
            return false;
        range = {cursor, size};
        cursor += size;
        return true;
    };
    if (!addStream(asset.vertices.size(), sizeof(StaticSceneVertex), layout.vertices) ||
        !addStream(asset.meshletVertices.size(), sizeof(std::uint32_t),
            layout.meshletVertices) ||
        !addStream(asset.meshletTriangles.size(), sizeof(std::uint8_t),
            layout.meshletTriangles) ||
        !addStream(asset.indices.size(), sizeof(std::uint32_t), layout.indices) ||
        !alignPage(cursor, pageSize, layout.rawSize) || layout.rawSize == 0u ||
        layout.rawSize / pageSize > std::numeric_limits<std::uint32_t>::max())
        return pageError<VirtualGeometryPageLayout>(Halcyon::ErrorCode::InvalidArgument,
            "virtual geometry stream layout exceeds the supported address range");
    layout.pageCount = static_cast<std::uint32_t>(layout.rawSize / pageSize);

    try
    {
        layout.nodeDependencies.resize(asset.dagNodes.size());
        std::vector<std::uint32_t> rootPages;
        for (std::uint32_t nodeIndex = 0u; nodeIndex < asset.dagNodes.size(); ++nodeIndex)
        {
            const auto& node = asset.dagNodes[nodeIndex];
            if (node.clusterIndex >= asset.clusters.size())
                return pageError<VirtualGeometryPageLayout>(
                    Halcyon::ErrorCode::InvalidArgument,
                    "DAG node references a missing Cluster");
            const auto& cluster = asset.clusters[node.clusterIndex];
            std::vector<std::uint32_t> dependencies;
            for (const std::uint32_t meshletIndex : cluster.meshletIndices)
            {
                if (meshletIndex >= asset.meshlets.size())
                    return pageError<VirtualGeometryPageLayout>(
                        Halcyon::ErrorCode::InvalidArgument,
                        "Cluster references a missing Meshlet");
                const auto& meshlet = asset.meshlets[meshletIndex];
                if (static_cast<std::size_t>(meshlet.vertexOffset) + meshlet.vertexCount >
                        asset.meshletVertices.size() ||
                    static_cast<std::size_t>(meshlet.triangleOffset) + meshlet.indexCount >
                        asset.meshletTriangles.size() ||
                    static_cast<std::size_t>(meshlet.indexOffset) + meshlet.indexCount >
                        asset.indices.size())
                    return pageError<VirtualGeometryPageLayout>(
                        Halcyon::ErrorCode::InvalidArgument,
                        "Meshlet pageable stream range is invalid");

                appendPageRange(dependencies,
                    layout.meshletVertices.offset +
                        static_cast<std::uint64_t>(meshlet.vertexOffset) * sizeof(std::uint32_t),
                    static_cast<std::uint64_t>(meshlet.vertexCount) * sizeof(std::uint32_t),
                    pageSize);
                appendPageRange(dependencies,
                    layout.meshletTriangles.offset + meshlet.triangleOffset,
                    meshlet.indexCount, pageSize);
                appendPageRange(dependencies,
                    layout.indices.offset +
                        static_cast<std::uint64_t>(meshlet.indexOffset) * sizeof(std::uint32_t),
                    static_cast<std::uint64_t>(meshlet.indexCount) * sizeof(std::uint32_t),
                    pageSize);
                for (std::uint32_t local = 0u; local < meshlet.vertexCount; ++local)
                {
                    const std::uint32_t vertexIndex =
                        asset.meshletVertices[meshlet.vertexOffset + local];
                    if (vertexIndex >= asset.vertices.size())
                        return pageError<VirtualGeometryPageLayout>(
                            Halcyon::ErrorCode::InvalidArgument,
                            "Meshlet references a missing vertex");
                    appendPageRange(dependencies,
                        layout.vertices.offset +
                            static_cast<std::uint64_t>(vertexIndex) * sizeof(StaticSceneVertex),
                        sizeof(StaticSceneVertex), pageSize);
                }
            }
            std::sort(dependencies.begin(), dependencies.end());
            dependencies.erase(std::unique(dependencies.begin(), dependencies.end()),
                dependencies.end());
            if (layout.dependencyPageIndices.size() >
                std::numeric_limits<std::uint32_t>::max() - dependencies.size())
                return pageError<VirtualGeometryPageLayout>(
                    Halcyon::ErrorCode::InvalidArgument,
                    "page dependency table exceeds the integer range");
            layout.nodeDependencies[nodeIndex] = {
                static_cast<std::uint32_t>(layout.dependencyPageIndices.size()),
                static_cast<std::uint32_t>(dependencies.size())};
            layout.dependencyPageIndices.insert(layout.dependencyPageIndices.end(),
                dependencies.begin(), dependencies.end());
            if (node.parentIndex == std::numeric_limits<std::uint32_t>::max())
                rootPages.insert(rootPages.end(), dependencies.begin(), dependencies.end());
        }
        std::sort(rootPages.begin(), rootPages.end());
        rootPages.erase(std::unique(rootPages.begin(), rootPages.end()), rootPages.end());
        layout.rootPageIndices = std::move(rootPages);
    }
    catch (const std::bad_alloc&)
    {
        return pageError<VirtualGeometryPageLayout>(Halcyon::ErrorCode::OutOfMemory,
            "unable to allocate virtual geometry page dependencies");
    }
    return Halcyon::Result<VirtualGeometryPageLayout>::success(std::move(layout));
}

Halcyon::Result<std::vector<std::byte>> serializeVirtualGeometryPage(
    const VirtualGeometryAsset& asset, const VirtualGeometryPageLayout& layout,
    std::uint32_t pageIndex)
{
    if (pageIndex >= layout.pageCount || layout.pageSize == 0u ||
        layout.rawSize != static_cast<std::uint64_t>(layout.pageCount) * layout.pageSize)
        return pageError<std::vector<std::byte>>(Halcyon::ErrorCode::InvalidArgument,
            "page index or stream layout is invalid");
    std::vector<std::byte> page;
    try { page.assign(layout.pageSize, std::byte{0}); }
    catch (const std::bad_alloc&)
    {
        return pageError<std::vector<std::byte>>(Halcyon::ErrorCode::OutOfMemory,
            "unable to allocate virtual geometry page");
    }
    const std::uint64_t pageOffset = static_cast<std::uint64_t>(pageIndex) * layout.pageSize;
    copyPageOverlap(page, pageOffset, layout.vertices, asset.vertices.data());
    copyPageOverlap(page, pageOffset, layout.meshletVertices,
        asset.meshletVertices.data());
    copyPageOverlap(page, pageOffset, layout.meshletTriangles,
        asset.meshletTriangles.data());
    copyPageOverlap(page, pageOffset, layout.indices, asset.indices.data());
    return Halcyon::Result<std::vector<std::byte>>::success(std::move(page));
}

bool virtualGeometryNodeDependsOnPage(const VirtualGeometryPageLayout& layout,
    std::uint32_t nodeIndex, std::uint32_t pageIndex) noexcept
{
    if (nodeIndex >= layout.nodeDependencies.size())
        return false;
    const auto range = layout.nodeDependencies[nodeIndex];
    if (static_cast<std::size_t>(range.offset) > layout.dependencyPageIndices.size() ||
        static_cast<std::size_t>(range.count) >
            layout.dependencyPageIndices.size() - range.offset)
        return false;
    const auto first = layout.dependencyPageIndices.begin() + range.offset;
    return std::binary_search(first, first + range.count, pageIndex);
}

} // namespace Halcyon::Renderer::Scene
