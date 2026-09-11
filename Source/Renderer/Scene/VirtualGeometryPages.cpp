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

bool alignUp(std::uint64_t value, std::uint32_t alignment,
    std::uint64_t& result) noexcept
{
    const std::uint64_t mask = alignment - 1u;
    if (alignment == 0u || (alignment & mask) != 0u ||
        value > std::numeric_limits<std::uint64_t>::max() - mask)
        return false;
    result = (value + mask) & ~mask;
    return true;
}

bool packedPageBytes(std::uint32_t vertexCount, std::uint32_t meshletVertexCount,
    std::uint32_t triangleByteCount, std::uint64_t& result) noexcept
{
    std::uint64_t cursor = kVirtualGeometryPageHeaderSize;
    std::uint64_t size = 0u;
    if (!checkedByteSize(vertexCount, sizeof(StaticSceneVertex), size) ||
        cursor > std::numeric_limits<std::uint64_t>::max() - size)
        return false;
    cursor += size;
    if (!checkedByteSize(vertexCount, sizeof(std::uint32_t), size) ||
        cursor > std::numeric_limits<std::uint64_t>::max() - size)
        return false;
    cursor += size;
    if (!checkedByteSize(meshletVertexCount, sizeof(std::uint32_t), size) ||
        cursor > std::numeric_limits<std::uint64_t>::max() - size)
        return false;
    cursor += size;
    if (cursor > std::numeric_limits<std::uint64_t>::max() - triangleByteCount)
        return false;
    cursor += triangleByteCount;
    if (!alignUp(cursor, 4u, cursor))
        return false;
    if (!checkedByteSize(triangleByteCount, sizeof(std::uint32_t), size) ||
        cursor > std::numeric_limits<std::uint64_t>::max() - size)
        return false;
    result = cursor + size;
    return true;
}

std::uint32_t findPackedVertex(const std::vector<std::uint32_t>& packed,
    std::uint32_t globalIndex) noexcept
{
    for (std::uint32_t i = 0u; i < packed.size(); ++i)
    {
        if (packed[i] == globalIndex)
            return i;
    }
    return std::numeric_limits<std::uint32_t>::max();
}

bool appendUniqueVertices(const VirtualGeometryAsset& asset,
    const VirtualGeometryMeshlet& meshlet, std::vector<std::uint32_t>& packed,
    std::uint32_t& added)
{
    added = 0u;
    if (!packed.empty() &&
        packed.size() > std::numeric_limits<std::uint32_t>::max() - meshlet.vertexCount)
        return false;
    for (std::uint32_t local = 0u; local < meshlet.vertexCount; ++local)
    {
        const std::uint32_t global =
            asset.meshletVertices[meshlet.vertexOffset + local];
        if (findPackedVertex(packed, global) != std::numeric_limits<std::uint32_t>::max())
            continue;
        packed.push_back(global);
        ++added;
    }
    return true;
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
    std::uint64_t vertexBytes = 0u;
    std::uint64_t meshletVertexBytes = 0u;
    std::uint64_t triangleBytes = 0u;
    std::uint64_t indexBytes = 0u;
    if (!checkedByteSize(asset.vertices.size(), sizeof(StaticSceneVertex), vertexBytes) ||
        !checkedByteSize(asset.meshletVertices.size(), sizeof(std::uint32_t),
            meshletVertexBytes) ||
        !checkedByteSize(asset.meshletTriangles.size(), sizeof(std::uint8_t),
            triangleBytes) ||
        !checkedByteSize(asset.indices.size(), sizeof(std::uint32_t), indexBytes))
        return pageError<VirtualGeometryPageLayout>(Halcyon::ErrorCode::InvalidArgument,
            "virtual geometry stream layout exceeds the supported address range");
    layout.vertices = {0u, vertexBytes};
    layout.meshletVertices = {vertexBytes, meshletVertexBytes};
    layout.meshletTriangles = {vertexBytes + meshletVertexBytes, triangleBytes};
    layout.indices = {vertexBytes + meshletVertexBytes + triangleBytes, indexBytes};

    try
    {
        layout.meshletAddresses.assign(asset.meshlets.size(), {});
        layout.nodeDependencies.resize(asset.dagNodes.size());
        std::vector<std::uint32_t> meshletAssigned(asset.meshlets.size(),
            std::numeric_limits<std::uint32_t>::max());
        std::vector<std::uint32_t> clusterFirstPage(asset.clusters.size(),
            std::numeric_limits<std::uint32_t>::max());
        std::vector<std::uint32_t> clusterPageCount(asset.clusters.size(), 0u);

        struct OpenPage
        {
            std::vector<std::uint32_t> packedVertices;
            std::uint32_t meshletVertexCount = 0u;
            std::uint32_t triangleByteCount = 0u;
            std::uint32_t meshletCount = 0u;
        };

        const auto flushPage = [&](OpenPage& open) -> bool
        {
            if (open.meshletCount == 0u)
                return true;
            if (layout.pageCount == std::numeric_limits<std::uint32_t>::max())
                return false;
            VirtualGeometryPageDescriptor page{};
            page.meshletOffset = static_cast<std::uint32_t>(layout.pageMeshlets.size()) -
                open.meshletCount;
            page.meshletCount = open.meshletCount;
            page.vertexCount = static_cast<std::uint32_t>(open.packedVertices.size());
            page.meshletVertexCount = open.meshletVertexCount;
            page.triangleByteCount = open.triangleByteCount;
            layout.pages.push_back(page);
            ++layout.pageCount;
            open = {};
            return true;
        };

        OpenPage open{};
        for (std::uint32_t clusterIndex = 0u; clusterIndex < asset.clusters.size();
             ++clusterIndex)
        {
            const auto& cluster = asset.clusters[clusterIndex];
            std::uint32_t firstPage = std::numeric_limits<std::uint32_t>::max();
            for (const std::uint32_t meshletIndex : cluster.meshletIndices)
            {
                if (meshletIndex >= asset.meshlets.size() ||
                    meshletAssigned[meshletIndex] != std::numeric_limits<std::uint32_t>::max())
                    return pageError<VirtualGeometryPageLayout>(
                        Halcyon::ErrorCode::InvalidArgument,
                        "Cluster references a missing or duplicated Meshlet");
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

                std::vector<std::uint32_t> candidate = open.packedVertices;
                std::uint32_t addedVertices = 0u;
                if (!appendUniqueVertices(asset, meshlet, candidate, addedVertices))
                    return pageError<VirtualGeometryPageLayout>(
                        Halcyon::ErrorCode::InvalidArgument,
                        "page vertex table exceeds the integer range");
                if (open.meshletVertexCount >
                        std::numeric_limits<std::uint32_t>::max() - meshlet.vertexCount ||
                    open.triangleByteCount >
                        std::numeric_limits<std::uint32_t>::max() - meshlet.indexCount)
                    return pageError<VirtualGeometryPageLayout>(
                        Halcyon::ErrorCode::InvalidArgument,
                        "page Meshlet payload exceeds the integer range");
                std::uint64_t packedBytes = 0u;
                if (!packedPageBytes(static_cast<std::uint32_t>(candidate.size()),
                        open.meshletVertexCount + meshlet.vertexCount,
                        open.triangleByteCount + meshlet.indexCount, packedBytes) ||
                    packedBytes > pageSize)
                {
                    if (!flushPage(open))
                        return pageError<VirtualGeometryPageLayout>(
                            Halcyon::ErrorCode::InvalidArgument,
                            "virtual geometry page count exceeds the integer range");
                    candidate.clear();
                    if (!appendUniqueVertices(asset, meshlet, candidate, addedVertices) ||
                        !packedPageBytes(static_cast<std::uint32_t>(candidate.size()),
                            meshlet.vertexCount, meshlet.indexCount, packedBytes) ||
                        packedBytes > pageSize)
                        return pageError<VirtualGeometryPageLayout>(
                            Halcyon::ErrorCode::InvalidArgument,
                            "Meshlet payload exceeds the virtual geometry page size");
                }

                VirtualGeometryPackedMeshletAddress address{};
                address.pageIndex = layout.pageCount;
                address.vertexOffset = open.meshletVertexCount;
                address.triangleOffset = open.triangleByteCount;
                layout.meshletAddresses[meshletIndex] = address;
                layout.pageMeshlets.push_back({meshletIndex, address.vertexOffset,
                    address.triangleOffset});
                open.packedVertices = std::move(candidate);
                open.meshletVertexCount += meshlet.vertexCount;
                open.triangleByteCount += meshlet.indexCount;
                ++open.meshletCount;
                meshletAssigned[meshletIndex] = clusterIndex;
                if (firstPage == std::numeric_limits<std::uint32_t>::max())
                    firstPage = layout.pageCount;
            }
            if (firstPage == std::numeric_limits<std::uint32_t>::max() ||
                layout.pageCount < firstPage ||
                (open.meshletCount == 0u && layout.pageCount == firstPage))
                return pageError<VirtualGeometryPageLayout>(
                    Halcyon::ErrorCode::InvalidArgument,
                    "Cluster produced no geometry pages");
            clusterFirstPage[clusterIndex] = firstPage;
            clusterPageCount[clusterIndex] = layout.pageCount - firstPage +
                (open.meshletCount != 0u ? 1u : 0u);
        }
        if (!flushPage(open))
            return pageError<VirtualGeometryPageLayout>(
                Halcyon::ErrorCode::InvalidArgument,
                "virtual geometry page count exceeds the integer range");

        for (const auto assigned : meshletAssigned)
        {
            if (assigned == std::numeric_limits<std::uint32_t>::max())
                return pageError<VirtualGeometryPageLayout>(
                    Halcyon::ErrorCode::InvalidArgument,
                    "Meshlet is not owned by any Cluster page");
        }

        std::vector<std::uint32_t> rootPages;
        for (std::uint32_t nodeIndex = 0u; nodeIndex < asset.dagNodes.size(); ++nodeIndex)
        {
            const auto& node = asset.dagNodes[nodeIndex];
            if (node.clusterIndex >= asset.clusters.size())
                return pageError<VirtualGeometryPageLayout>(
                    Halcyon::ErrorCode::InvalidArgument,
                    "DAG node references a missing Cluster");
            const std::uint32_t first = clusterFirstPage[node.clusterIndex];
            const std::uint32_t count = clusterPageCount[node.clusterIndex];
            if (layout.dependencyPageIndices.size() >
                std::numeric_limits<std::uint32_t>::max() - count)
                return pageError<VirtualGeometryPageLayout>(
                    Halcyon::ErrorCode::InvalidArgument,
                    "page dependency table exceeds the integer range");
            layout.nodeDependencies[nodeIndex] = {
                static_cast<std::uint32_t>(layout.dependencyPageIndices.size()), count};
            for (std::uint32_t i = 0u; i < count; ++i)
                layout.dependencyPageIndices.push_back(first + i);
            if (node.parentIndex == std::numeric_limits<std::uint32_t>::max())
            {
                for (std::uint32_t i = 0u; i < count; ++i)
                    rootPages.push_back(first + i);
            }
        }
        std::sort(rootPages.begin(), rootPages.end());
        rootPages.erase(std::unique(rootPages.begin(), rootPages.end()), rootPages.end());
        layout.rootPageIndices = std::move(rootPages);
        if (layout.pageCount == 0u ||
            !checkedByteSize(layout.pageCount, pageSize, layout.rawSize))
            return pageError<VirtualGeometryPageLayout>(Halcyon::ErrorCode::InvalidArgument,
                "virtual geometry page layout exceeds the supported address range");
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
    if (pageIndex >= layout.pageCount || pageIndex >= layout.pages.size() ||
        layout.pageSize == 0u ||
        layout.rawSize != static_cast<std::uint64_t>(layout.pageCount) * layout.pageSize)
        return pageError<std::vector<std::byte>>(Halcyon::ErrorCode::InvalidArgument,
            "page index or stream layout is invalid");
    const auto& page = layout.pages[pageIndex];
    if (static_cast<std::size_t>(page.meshletOffset) > layout.pageMeshlets.size() ||
        static_cast<std::size_t>(page.meshletCount) >
            layout.pageMeshlets.size() - page.meshletOffset)
        return pageError<std::vector<std::byte>>(Halcyon::ErrorCode::InvalidArgument,
            "page Meshlet table is invalid");

    std::vector<std::byte> bytes;
    try
    {
        bytes.assign(layout.pageSize, std::byte{0});
    }
    catch (const std::bad_alloc&)
    {
        return pageError<std::vector<std::byte>>(Halcyon::ErrorCode::OutOfMemory,
            "unable to allocate virtual geometry page");
    }

    std::vector<std::uint32_t> packedVertices;
    std::vector<std::uint32_t> packedMeshletVertices;
    std::vector<std::uint8_t> packedTriangles;
    std::vector<std::uint32_t> packedIndices;
    try
    {
        packedVertices.reserve(page.vertexCount);
        packedMeshletVertices.reserve(page.meshletVertexCount);
        packedTriangles.reserve(page.triangleByteCount);
        packedIndices.reserve(page.triangleByteCount);
        for (std::uint32_t i = 0u; i < page.meshletCount; ++i)
        {
            const auto& packed = layout.pageMeshlets[page.meshletOffset + i];
            if (packed.meshletIndex >= asset.meshlets.size() ||
                packed.meshletIndex >= layout.meshletAddresses.size())
                return pageError<std::vector<std::byte>>(Halcyon::ErrorCode::InvalidArgument,
                    "page references a missing Meshlet");
            const auto& meshlet = asset.meshlets[packed.meshletIndex];
            const auto& address = layout.meshletAddresses[packed.meshletIndex];
            if (address.pageIndex != pageIndex ||
                address.vertexOffset != packed.vertexOffset ||
                address.triangleOffset != packed.triangleOffset ||
                packed.vertexOffset != packedMeshletVertices.size() ||
                packed.triangleOffset != packedTriangles.size())
                return pageError<std::vector<std::byte>>(Halcyon::ErrorCode::InvalidArgument,
                    "page Meshlet address is inconsistent");
            for (std::uint32_t local = 0u; local < meshlet.vertexCount; ++local)
            {
                const std::uint32_t global =
                    asset.meshletVertices[meshlet.vertexOffset + local];
                if (global >= asset.vertices.size())
                    return pageError<std::vector<std::byte>>(
                        Halcyon::ErrorCode::InvalidArgument,
                        "Meshlet references a missing vertex");
                std::uint32_t packedIndex = findPackedVertex(packedVertices, global);
                if (packedIndex == std::numeric_limits<std::uint32_t>::max())
                {
                    packedIndex = static_cast<std::uint32_t>(packedVertices.size());
                    packedVertices.push_back(global);
                }
                packedMeshletVertices.push_back(packedIndex);
            }
            for (std::uint32_t corner = 0u; corner < meshlet.indexCount; ++corner)
            {
                const std::uint8_t local =
                    asset.meshletTriangles[meshlet.triangleOffset + corner];
                if (local >= meshlet.vertexCount)
                    return pageError<std::vector<std::byte>>(
                        Halcyon::ErrorCode::InvalidArgument,
                        "Meshlet triangle references a missing local vertex");
                packedTriangles.push_back(local);
                packedIndices.push_back(
                    packedMeshletVertices[packed.vertexOffset + local]);
            }
        }
    }
    catch (const std::bad_alloc&)
    {
        return pageError<std::vector<std::byte>>(Halcyon::ErrorCode::OutOfMemory,
            "unable to pack virtual geometry page");
    }

    if (packedVertices.size() != page.vertexCount ||
        packedMeshletVertices.size() != page.meshletVertexCount ||
        packedTriangles.size() != page.triangleByteCount ||
        packedIndices.size() != page.triangleByteCount)
        return pageError<std::vector<std::byte>>(Halcyon::ErrorCode::InvalidArgument,
            "serialized page payload does not match its descriptor");

    std::uint64_t packedBytes = 0u;
    if (!packedPageBytes(page.vertexCount, page.meshletVertexCount, page.triangleByteCount,
            packedBytes) ||
        packedBytes > layout.pageSize)
        return pageError<std::vector<std::byte>>(Halcyon::ErrorCode::InvalidArgument,
            "serialized page payload exceeds the page size");

    const auto write32 = [&](std::size_t offset, std::uint32_t value)
    {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    };
    write32(0u, page.meshletCount);
    write32(4u, page.vertexCount);
    write32(8u, page.meshletVertexCount);
    write32(12u, page.triangleByteCount);
    std::size_t cursor = kVirtualGeometryPageHeaderSize;
    if (page.vertexCount != 0u)
    {
        for (std::uint32_t i = 0u; i < page.vertexCount; ++i)
        {
            std::memcpy(bytes.data() + cursor, &asset.vertices[packedVertices[i]],
                sizeof(StaticSceneVertex));
            cursor += sizeof(StaticSceneVertex);
        }
        std::memcpy(bytes.data() + cursor, packedVertices.data(),
            packedVertices.size() * sizeof(std::uint32_t));
        cursor += packedVertices.size() * sizeof(std::uint32_t);
    }
    if (page.meshletVertexCount != 0u)
    {
        std::memcpy(bytes.data() + cursor, packedMeshletVertices.data(),
            packedMeshletVertices.size() * sizeof(std::uint32_t));
        cursor += packedMeshletVertices.size() * sizeof(std::uint32_t);
    }
    if (page.triangleByteCount != 0u)
    {
        std::memcpy(bytes.data() + cursor, packedTriangles.data(), packedTriangles.size());
        cursor += packedTriangles.size();
        cursor = (cursor + 3u) & ~std::size_t{3u};
        std::memcpy(bytes.data() + cursor, packedIndices.data(),
            packedIndices.size() * sizeof(std::uint32_t));
    }
    return Halcyon::Result<std::vector<std::byte>>::success(std::move(bytes));
}

Halcyon::Result<void> unpackVirtualGeometryPage(VirtualGeometryAsset& asset,
    const VirtualGeometryPageLayout& layout, std::uint32_t pageIndex,
    std::span<const std::byte> page)
{
    if (pageIndex >= layout.pageCount || pageIndex >= layout.pages.size() ||
        page.size() != layout.pageSize)
        return pageError<void>(Halcyon::ErrorCode::InvalidArgument,
            "page index or size is invalid");
    const auto& descriptor = layout.pages[pageIndex];
    if (static_cast<std::size_t>(descriptor.meshletOffset) > layout.pageMeshlets.size() ||
        static_cast<std::size_t>(descriptor.meshletCount) >
            layout.pageMeshlets.size() - descriptor.meshletOffset)
        return pageError<void>(Halcyon::ErrorCode::InvalidArgument,
            "page Meshlet table is invalid");

    const auto read32 = [&](std::size_t offset, std::uint32_t& value) noexcept
    {
        if (offset > page.size() || page.size() - offset < sizeof(std::uint32_t))
            return false;
        std::memcpy(&value, page.data() + offset, sizeof(value));
        return true;
    };
    std::uint32_t meshletCount = 0u, vertexCount = 0u, meshletVertexCount = 0u;
    std::uint32_t triangleByteCount = 0u;
    if (!read32(0u, meshletCount) || !read32(4u, vertexCount) ||
        !read32(8u, meshletVertexCount) || !read32(12u, triangleByteCount))
        return pageError<void>(Halcyon::ErrorCode::InvalidArgument,
            "page header is truncated");
    if (meshletCount != descriptor.meshletCount || vertexCount != descriptor.vertexCount ||
        meshletVertexCount != descriptor.meshletVertexCount ||
        triangleByteCount != descriptor.triangleByteCount)
        return pageError<void>(Halcyon::ErrorCode::InvalidArgument,
            "page header does not match its descriptor");

    std::uint64_t packedBytes = 0u;
    if (!packedPageBytes(vertexCount, meshletVertexCount, triangleByteCount, packedBytes) ||
        packedBytes > page.size())
        return pageError<void>(Halcyon::ErrorCode::InvalidArgument,
            "page payload exceeds the page size");

    std::vector<StaticSceneVertex> packedVertices;
    std::vector<std::uint32_t> remap;
    std::vector<std::uint32_t> packedMeshletVertices;
    std::vector<std::uint8_t> packedTriangles;
    std::vector<std::uint32_t> packedIndices;
    try
    {
        packedVertices.resize(vertexCount);
        remap.resize(vertexCount);
        packedMeshletVertices.resize(meshletVertexCount);
        packedTriangles.resize(triangleByteCount);
        packedIndices.resize(triangleByteCount);
    }
    catch (const std::bad_alloc&)
    {
        return pageError<void>(Halcyon::ErrorCode::OutOfMemory,
            "unable to unpack virtual geometry page");
    }

    std::size_t cursor = kVirtualGeometryPageHeaderSize;
    if (vertexCount != 0u)
    {
        std::memcpy(packedVertices.data(), page.data() + cursor,
            packedVertices.size() * sizeof(StaticSceneVertex));
        cursor += packedVertices.size() * sizeof(StaticSceneVertex);
        std::memcpy(remap.data(), page.data() + cursor,
            remap.size() * sizeof(std::uint32_t));
        cursor += remap.size() * sizeof(std::uint32_t);
    }
    if (meshletVertexCount != 0u)
    {
        std::memcpy(packedMeshletVertices.data(), page.data() + cursor,
            packedMeshletVertices.size() * sizeof(std::uint32_t));
        cursor += packedMeshletVertices.size() * sizeof(std::uint32_t);
    }
    if (triangleByteCount != 0u)
    {
        std::memcpy(packedTriangles.data(), page.data() + cursor, packedTriangles.size());
        cursor += packedTriangles.size();
        cursor = (cursor + 3u) & ~std::size_t{3u};
        if (cursor > page.size() ||
            page.size() - cursor < packedIndices.size() * sizeof(std::uint32_t))
            return pageError<void>(Halcyon::ErrorCode::InvalidArgument,
                "page index payload is truncated");
        std::memcpy(packedIndices.data(), page.data() + cursor,
            packedIndices.size() * sizeof(std::uint32_t));
    }

    for (std::uint32_t i = 0u; i < vertexCount; ++i)
    {
        if (remap[i] >= asset.vertices.size())
            return pageError<void>(Halcyon::ErrorCode::InvalidArgument,
                "page remap references a missing vertex");
        asset.vertices[remap[i]] = packedVertices[i];
    }

    std::size_t meshletVertexCursor = 0u;
    std::size_t triangleCursor = 0u;
    for (std::uint32_t i = 0u; i < meshletCount; ++i)
    {
        const auto& packed = layout.pageMeshlets[descriptor.meshletOffset + i];
        if (packed.meshletIndex >= asset.meshlets.size() ||
            packed.meshletIndex >= layout.meshletAddresses.size())
            return pageError<void>(Halcyon::ErrorCode::InvalidArgument,
                "page references a missing Meshlet");
        const auto& meshlet = asset.meshlets[packed.meshletIndex];
        const auto& address = layout.meshletAddresses[packed.meshletIndex];
        if (address.pageIndex != pageIndex ||
            address.vertexOffset != packed.vertexOffset ||
            address.triangleOffset != packed.triangleOffset ||
            packed.vertexOffset != meshletVertexCursor ||
            packed.triangleOffset != triangleCursor ||
            static_cast<std::size_t>(meshlet.vertexOffset) + meshlet.vertexCount >
                asset.meshletVertices.size() ||
            static_cast<std::size_t>(meshlet.triangleOffset) + meshlet.indexCount >
                asset.meshletTriangles.size() ||
            static_cast<std::size_t>(meshlet.indexOffset) + meshlet.indexCount >
                asset.indices.size())
            return pageError<void>(Halcyon::ErrorCode::InvalidArgument,
                "unpacked Meshlet address is inconsistent");
        for (std::uint32_t local = 0u; local < meshlet.vertexCount; ++local)
        {
            const std::uint32_t packedIndex = packedMeshletVertices[meshletVertexCursor++];
            if (packedIndex >= vertexCount)
                return pageError<void>(Halcyon::ErrorCode::InvalidArgument,
                    "page Meshlet vertex is out of range");
            asset.meshletVertices[meshlet.vertexOffset + local] = remap[packedIndex];
        }
        for (std::uint32_t corner = 0u; corner < meshlet.indexCount; ++corner)
        {
            const std::uint8_t local = packedTriangles[triangleCursor];
            const std::uint32_t packedIndex = packedIndices[triangleCursor++];
            if (local >= meshlet.vertexCount || packedIndex >= vertexCount)
                return pageError<void>(Halcyon::ErrorCode::InvalidArgument,
                    "page triangle index is out of range");
            asset.meshletTriangles[meshlet.triangleOffset + corner] = local;
            asset.indices[meshlet.indexOffset + corner] = remap[packedIndex];
        }
    }
    if (meshletVertexCursor != meshletVertexCount || triangleCursor != triangleByteCount)
        return pageError<void>(Halcyon::ErrorCode::InvalidArgument,
            "unpacked page payload does not match its descriptor");
    return Halcyon::Result<void>::success();
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
