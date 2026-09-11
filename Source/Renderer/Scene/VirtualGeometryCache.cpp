#include "VirtualGeometryCache.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <zstd.h>
#include <meshoptimizer.h>
#include <metis.h>

namespace Halcyon::Renderer::Scene
{
namespace
{

constexpr std::array<char, 8> kMagic{'H', 'A', 'L', 'C', 'Y', 'O', 'N', 'V'};
constexpr std::uint32_t kEndian = 0x01020304u;
constexpr std::uint32_t kResidentMetadataMagic = 0x354d4756u; // VGM5
constexpr std::uint32_t kResidentMetadataVersion = 3u;
constexpr std::size_t kMaxVisibilityMeshlets = kVirtualVisibilityMeshletMask;
constexpr std::uint64_t kHeaderSize = 128u;
constexpr std::uint64_t kPageDirectoryEntrySize = 32u;
constexpr std::uint64_t kMaxResidentMetadataBytes = 512ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaxCachePayloadBytes = 16ull * 1024ull * 1024ull * 1024ull;
constexpr std::uint32_t kMaxCachePageCount = 1u << 20u;

std::uint32_t dagChecksum(const VirtualGeometryAsset& asset) noexcept
{
    std::uint32_t hash = 2166136261u;
    const auto mix = [&](std::uint32_t value) { hash ^= value; hash *= 16777619u; };
    for (const auto& node : asset.dagNodes)
    {
        mix(node.clusterIndex); mix(node.parentIndex); mix(node.firstChild);
        mix(node.childCount); mix(node.lodDepth); mix(node.flags);
        std::uint32_t bits = 0; std::memcpy(&bits, &node.geometricError, sizeof(bits)); mix(bits);
    }
    for (const auto& edge : asset.dagEdges) { mix(edge.parent); mix(edge.child); }
    for (const auto& cluster : asset.clusters)
    {
        for (const auto adjacent : cluster.adjacentClusters)
            mix(adjacent);
    }
    return hash;
}

void append32(std::vector<std::byte>& out, std::uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>(value >> (i * 8u)));
}
void append64(std::vector<std::byte>& out, std::uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) out.push_back(static_cast<std::byte>(value >> (i * 8u)));
}
void appendFloat(std::vector<std::byte>& out, float value)
{
    std::uint32_t bits = 0; std::memcpy(&bits, &value, sizeof(bits)); append32(out, bits);
}
void appendBytes(std::vector<std::byte>& out, const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const std::byte*>(data); out.insert(out.end(), bytes, bytes + size);
}

struct Reader
{
    std::span<const std::byte> data; std::size_t offset = 0;
    bool read32(std::uint32_t& value)
    {
        if (offset > data.size() || data.size() - offset < 4u) return false; value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(data[offset++]) << (i * 8u); return true;
    }
    bool read64(std::uint64_t& value)
    {
        if (offset > data.size() || data.size() - offset < 8u) return false; value = 0;
        for (unsigned i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(data[offset++]) << (i * 8u); return true;
    }
    bool readFloat(float& value) { std::uint32_t bits = 0; if (!read32(bits)) return false; std::memcpy(&value, &bits, sizeof(value)); return true; }
    bool readBytes(void* destination, std::size_t size)
    {
        if (offset > data.size() || size > data.size() - offset) return false; std::memcpy(destination, data.data() + offset, size); offset += size; return true;
    }
};

Halcyon::Result<VirtualGeometryAsset> cacheError(std::string message)
{
    return Halcyon::Result<VirtualGeometryAsset>::failure(
        {Halcyon::ErrorCode::InvalidArgument, std::move(message), "VirtualGeometryCache"});
}

Halcyon::Result<VirtualGeometryAsset> cacheAllocationError()
{
    return Halcyon::Result<VirtualGeometryAsset>::failure(
        {Halcyon::ErrorCode::OutOfMemory,
            "unable to allocate validated cache tables", "VirtualGeometryCache"});
}

template <typename T>
Halcyon::Result<T> typedCacheError(std::string message)
{
    return Halcyon::Result<T>::failure(
        {Halcyon::ErrorCode::InvalidArgument, std::move(message), "VirtualGeometryCache"});
}

Halcyon::Result<void> cacheVoidError(std::string message)
{
    return Halcyon::Result<void>::failure(
        {Halcyon::ErrorCode::InvalidArgument, std::move(message),
            "VirtualGeometryCache"});
}

std::uint32_t crc32(std::span<const std::byte> bytes) noexcept
{
    std::uint32_t crc = 0xffffffffu;
    for (const std::byte value : bytes)
    {
        crc ^= static_cast<std::uint8_t>(value);
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1u) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

Halcyon::Result<std::vector<std::byte>> readCompressedPage(
    std::ifstream& stream, const VirtualGeometryCachePageEntry& entry)
{
    if (entry.compressedSize < 5u || entry.rawSize == 0u)
        return typedCacheError<std::vector<std::byte>>("cache page is truncated");
    std::vector<std::byte> compressed;
    std::vector<std::byte> raw;
    try
    {
        compressed.resize(entry.compressedSize);
        raw.resize(entry.rawSize);
    }
    catch (const std::bad_alloc&)
    {
        return Halcyon::Result<std::vector<std::byte>>::failure(
            {Halcyon::ErrorCode::OutOfMemory,
                "unable to allocate cache page", "VirtualGeometryCache"});
    }
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(entry.fileOffset));
    stream.read(reinterpret_cast<char*>(compressed.data()),
        static_cast<std::streamsize>(compressed.size()));
    if (!stream)
        return typedCacheError<std::vector<std::byte>>("unable to read cache page");

    const auto* bytes = reinterpret_cast<const std::uint8_t*>(compressed.data());
    constexpr std::uint32_t kZstdFrameMagic = 0xFD2FB528u;
    const std::uint32_t magic = static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8u) |
        (static_cast<std::uint32_t>(bytes[2]) << 16u) |
        (static_cast<std::uint32_t>(bytes[3]) << 24u);
    if (magic != kZstdFrameMagic || (bytes[4] & 0x04u) == 0u)
        return typedCacheError<std::vector<std::byte>>(
            "cache page is missing a zstd checksum");
    const auto frameSize = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
    if (frameSize == ZSTD_CONTENTSIZE_ERROR || frameSize == ZSTD_CONTENTSIZE_UNKNOWN ||
        frameSize != entry.rawSize)
        return typedCacheError<std::vector<std::byte>>("cache page frame size is invalid");
    const std::size_t result = ZSTD_decompress(raw.data(), raw.size(),
        compressed.data(), compressed.size());
    if (ZSTD_isError(result) || result != entry.rawSize || crc32(raw) != entry.crc32)
        return typedCacheError<std::vector<std::byte>>(
            "cache page checksum or size mismatch");
    return Halcyon::Result<std::vector<std::byte>>::success(std::move(raw));
}

std::optional<std::string_view> validateBuildOptions(
    const VirtualGeometryBuildOptions& options) noexcept
{
    if (options.maxVertices < 3u || options.maxVertices > kVirtualGeometryMaxVertices ||
        options.maxTriangles == 0u || options.maxTriangles > kVirtualGeometryMaxTriangles ||
        options.maxTriangles % 4u != 0u || !std::isfinite(options.simplifyError) ||
        options.simplifyError < 0.0f || options.maxClusterVertices < 3u ||
        options.maxClusterTriangles < 3u || options.metisUfactor == 0u ||
        options.metisUfactor > 1000u || options.maxDagDepth == 0u)
        return "cache build options exceed the M5 meshlet limits";

    bool disabledLodSeen = false;
    float previousRatio = 1.0f;
    for (std::size_t i = 0; i < options.lodRatios.size(); ++i)
    {
        const float ratio = options.lodRatios[i];
        if (!std::isfinite(ratio) || ratio < 0.0f || ratio > 1.0f ||
            (i == 0u && ratio != 1.0f) || ratio > previousRatio ||
            (disabledLodSeen && ratio != 0.0f))
            return "cache LOD ratios must start at 1, descend, and use trailing zeros";
        disabledLodSeen = disabledLodSeen || ratio == 0.0f;
        previousRatio = ratio;
    }
    return std::nullopt;
}

std::optional<std::string_view> validateAsset(
    const VirtualGeometryAsset& asset, const VirtualGeometryBuildOptions& options) noexcept
{
    const auto inRange = [](std::uint32_t offset, std::uint32_t count,
                             std::size_t size) noexcept
    {
        return static_cast<std::size_t>(offset) <= size &&
            static_cast<std::size_t>(count) <= size - offset;
    };
    const auto finiteVec3 = [](const glm::vec3& value) noexcept
    {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
            std::isfinite(value.z);
    };
    const auto finiteVec4 = [](const glm::vec4& value) noexcept
    {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
            std::isfinite(value.z) && std::isfinite(value.w);
    };

    if (asset.vertices.empty() || asset.meshlets.empty() || asset.lods.empty() ||
        asset.primitives.empty() || asset.meshlets.size() > kMaxVisibilityMeshlets)
        return "cache asset is empty or exceeds the visibility ID capacity";

    std::size_t primitiveVertexCursor = 0;
    for (const auto& primitive : asset.primitives)
    {
        if (primitive.vertexOffset != primitiveVertexCursor || primitive.vertexCount == 0u ||
            primitive.materialIndex > kVirtualVisibilityMaterialIndexMask ||
            !inRange(primitive.vertexOffset, primitive.vertexCount, asset.vertices.size()) ||
            !finiteVec3(primitive.boundsMin) || !finiteVec3(primitive.boundsMax) ||
            glm::any(glm::greaterThan(primitive.boundsMin, primitive.boundsMax)))
            return "cache primitive ranges or bounds are invalid";
        primitiveVertexCursor += primitive.vertexCount;
    }
    if (primitiveVertexCursor != asset.vertices.size())
        return "cache primitive ranges do not partition the vertex table";

    for (const auto& vertex : asset.vertices)
    {
        if (!finiteVec3(vertex.position) || !finiteVec3(vertex.normal) ||
            !std::isfinite(vertex.uv.x) || !std::isfinite(vertex.uv.y) ||
            !finiteVec4(vertex.tangent))
            return "cache vertex data contains a non-finite value";
    }

    std::size_t meshletVertexCursor = 0;
    std::size_t meshletTriangleCursor = 0;
    std::size_t meshletIndexCursor = 0;
    for (const auto& meshlet : asset.meshlets)
    {
        if (meshlet.vertexOffset != meshletVertexCursor ||
            meshlet.triangleOffset != meshletTriangleCursor ||
            meshlet.indexOffset != meshletIndexCursor || meshlet.vertexCount == 0u ||
            meshlet.triangleCount == 0u || meshlet.vertexCount > options.maxVertices ||
            meshlet.triangleCount > options.maxTriangles ||
            meshlet.indexCount != meshlet.triangleCount * 3u ||
            !inRange(meshlet.vertexOffset, meshlet.vertexCount,
                asset.meshletVertices.size()) ||
            !inRange(meshlet.triangleOffset, meshlet.indexCount,
                asset.meshletTriangles.size()) ||
            !inRange(meshlet.indexOffset, meshlet.indexCount, asset.indices.size()) ||
            meshlet.primitiveIndex >= asset.primitives.size() ||
            meshlet.lodIndex >= options.lodRatios.size() ||
            options.lodRatios[meshlet.lodIndex] == 0.0f ||
            !std::isfinite(meshlet.geometricError) || meshlet.geometricError < 0.0f ||
            !finiteVec4(meshlet.sphere) || meshlet.sphere.w < 0.0f ||
            !finiteVec4(meshlet.cone) || meshlet.cone.w < -1.0f || meshlet.cone.w > 1.0f)
            return "cache meshlet metadata is invalid";

        const auto& primitive = asset.primitives[meshlet.primitiveIndex];
        const std::size_t primitiveEnd =
            static_cast<std::size_t>(primitive.vertexOffset) + primitive.vertexCount;
        for (std::uint32_t vertex = 0; vertex < meshlet.vertexCount; ++vertex)
        {
            const std::uint32_t source =
                asset.meshletVertices[meshlet.vertexOffset + vertex];
            if (source < primitive.vertexOffset || source >= primitiveEnd)
                return "cache meshlet vertex escapes its primitive";
        }
        for (std::uint32_t index = 0; index < meshlet.indexCount; ++index)
        {
            const std::uint8_t local =
                asset.meshletTriangles[meshlet.triangleOffset + index];
            if (local >= meshlet.vertexCount)
                return "cache meshlet triangle references a missing local vertex";
            const std::uint32_t source =
                asset.meshletVertices[meshlet.vertexOffset + local];
            if (asset.indices[meshlet.indexOffset + index] != source)
                return "cache indexed and local meshlet topology disagree";
        }
        meshletVertexCursor += meshlet.vertexCount;
        meshletTriangleCursor += meshlet.indexCount;
        meshletIndexCursor += meshlet.indexCount;
    }
    if (meshletVertexCursor != asset.meshletVertices.size() ||
        meshletTriangleCursor != asset.meshletTriangles.size() ||
        meshletIndexCursor != asset.indices.size())
        return "cache meshlets do not partition their payload tables";

    std::size_t enabledLodCount = 0;
    for (const float ratio : options.lodRatios)
    {
        if (ratio > 0.0f)
            ++enabledLodCount;
    }
    if (enabledLodCount == 0u ||
        asset.primitives.size() > std::numeric_limits<std::size_t>::max() / enabledLodCount ||
        asset.lods.size() != enabledLodCount * asset.primitives.size())
        return "cache LOD table does not contain one entry for every enabled primitive LOD";

    std::size_t lodMeshletCursor = 0;
    std::size_t lodIndexCursor = 0;
    for (std::size_t lodIndex = 0; lodIndex < asset.lods.size(); ++lodIndex)
    {
        const auto& lod = asset.lods[lodIndex];
        const std::size_t primitiveIndex = lodIndex / enabledLodCount;
        const std::size_t enabledIndex = lodIndex % enabledLodCount;
        const float expectedRatio = [&]() noexcept
        {
            std::size_t seen = 0;
            for (const float ratio : options.lodRatios)
            {
                if (ratio > 0.0f)
                {
                    if (seen++ == enabledIndex)
                        return ratio;
                }
            }
            return 0.0f;
        }();
        if (lod.meshletOffset != lodMeshletCursor || lod.indexOffset != lodIndexCursor ||
            lod.meshletCount == 0u || lod.indexCount == 0u ||
            !inRange(lod.meshletOffset, lod.meshletCount, asset.meshlets.size()) ||
            !inRange(lod.indexOffset, lod.indexCount, asset.indices.size()) ||
            lod.primitiveIndex != primitiveIndex || lod.primitiveIndex >= asset.primitives.size() ||
            !std::isfinite(lod.ratio) ||
            lod.ratio <= 0.0f || lod.ratio > 1.0f ||
            lod.ratio != expectedRatio ||
            !std::isfinite(lod.geometricError) || lod.geometricError < 0.0f)
            return "cache LOD metadata is invalid";

        std::size_t expectedIndex = lod.indexOffset;
        for (std::uint32_t i = 0; i < lod.meshletCount; ++i)
        {
            const auto& meshlet = asset.meshlets[lod.meshletOffset + i];
            if (meshlet.primitiveIndex != lod.primitiveIndex ||
                options.lodRatios[meshlet.lodIndex] != lod.ratio ||
                meshlet.geometricError != lod.geometricError ||
                meshlet.indexOffset != expectedIndex)
                return "cache LOD and meshlet sequences disagree";
            expectedIndex += meshlet.indexCount;
        }
        if (expectedIndex != static_cast<std::size_t>(lod.indexOffset) + lod.indexCount)
            return "cache LOD index range does not match its meshlets";
        lodMeshletCursor += lod.meshletCount;
        lodIndexCursor += lod.indexCount;
    }
    if (lodMeshletCursor != asset.meshlets.size() || lodIndexCursor != asset.indices.size())
        return "cache LODs do not partition the meshlet and index tables";
    for (const auto& cluster : asset.clusters)
    {
        if (cluster.meshletCount != cluster.meshletIndices.size() || cluster.meshletCount == 0u ||
            cluster.triangleCount == 0u || cluster.triangleCount > options.maxClusterTriangles ||
            cluster.vertexCount > options.maxClusterVertices ||
            cluster.primitiveIndex >= asset.primitives.size())
            return "cache cluster meshlet index list is invalid";
        for (const auto meshletIndex : cluster.meshletIndices)
            if (meshletIndex >= asset.meshlets.size() ||
                asset.meshlets[meshletIndex].primitiveIndex != cluster.primitiveIndex)
                return "cache cluster references a missing or foreign meshlet";
        if (cluster.vertexCount == 0u || cluster.vertexOffset >= asset.vertices.size() ||
            cluster.vertexCount > asset.vertices.size())
            return "cache cluster vertex range is invalid";
        for (std::size_t i = 0; i < cluster.boundaryVertices.size(); ++i)
        {
            const auto boundary = cluster.boundaryVertices[i];
            if (boundary >= asset.vertices.size()) return "cache boundary vertex is out of range";
            if (i != 0u && cluster.boundaryVertices[i - 1u] >= boundary)
                return "cache boundary vertex table is not sorted and unique";
        }
        if (!std::isfinite(cluster.geometricError) || cluster.geometricError < 0.0f ||
            !finiteVec4(cluster.sphere) || cluster.sphere.w < 0.0f)
            return "cache cluster metadata is invalid";
        for (std::size_t i = 0; i < cluster.adjacentClusters.size(); ++i)
        {
            const auto adjacent = cluster.adjacentClusters[i];
            if (adjacent >= asset.clusters.size() || adjacent ==
                    static_cast<std::uint32_t>(&cluster - asset.clusters.data()) ||
                (i != 0u && cluster.adjacentClusters[i - 1u] >= adjacent))
                return "cache cluster adjacency is not sorted or in range";
            const auto& other = asset.clusters[adjacent];
            if (other.primitiveIndex != cluster.primitiveIndex ||
                other.lodDepth != cluster.lodDepth ||
                !std::binary_search(other.adjacentClusters.begin(),
                    other.adjacentClusters.end(),
                    static_cast<std::uint32_t>(&cluster - asset.clusters.data())))
                return "cache cluster adjacency is not symmetric";
        }
    }
    return std::nullopt;
}

bool checkedAdd(std::uint64_t left, std::uint64_t right,
    std::uint64_t& result) noexcept
{
    if (left > std::numeric_limits<std::uint64_t>::max() - right)
        return false;
    result = left + right;
    return true;
}

bool tableByteSize(std::uint64_t count, std::uint64_t stride,
    std::uint64_t& result) noexcept
{
    if (stride != 0u && count > std::numeric_limits<std::uint64_t>::max() / stride)
        return false;
    result = count * stride;
    return true;
}

Halcyon::Result<std::vector<std::byte>> serializeResidentMetadata(
    const VirtualGeometryAsset& asset, const VirtualGeometryCacheOptions& options,
    const VirtualGeometryPageLayout& layout)
{
    try
    {
        const auto fits32 = [](std::size_t value) noexcept
        {
            return value <= std::numeric_limits<std::uint32_t>::max();
        };
        std::size_t boundaryCount = 0u;
        std::size_t adjacencyCount = 0u;
        for (const auto& cluster : asset.clusters)
        {
            boundaryCount += cluster.boundaryVertices.size();
            adjacencyCount += cluster.adjacentClusters.size();
        }
        if (!fits32(asset.vertices.size()) || !fits32(asset.meshletVertices.size()) ||
            !fits32(asset.meshletTriangles.size()) || !fits32(asset.indices.size()) ||
            !fits32(asset.meshlets.size()) || !fits32(asset.lods.size()) ||
            !fits32(asset.primitives.size()) || !fits32(asset.clusters.size()) ||
            !fits32(asset.dagNodes.size()) || !fits32(asset.dagEdges.size()) ||
            !fits32(boundaryCount) || !fits32(adjacencyCount) ||
            !fits32(layout.nodeDependencies.size()) ||
            !fits32(layout.dependencyPageIndices.size()) ||
            !fits32(layout.meshletAddresses.size()) ||
            !fits32(layout.pages.size()) ||
            !fits32(layout.pageMeshlets.size()) ||
            layout.meshletAddresses.size() != asset.meshlets.size() ||
            layout.pages.size() != layout.pageCount)
            return typedCacheError<std::vector<std::byte>>(
                "resident metadata table exceeds the cache integer range");

        std::vector<std::byte> payload;
        append32(payload, kResidentMetadataMagic);
        append32(payload, kResidentMetadataVersion);
        append32(payload, static_cast<std::uint32_t>(asset.vertices.size()));
        append32(payload, static_cast<std::uint32_t>(asset.meshletVertices.size()));
        append32(payload, static_cast<std::uint32_t>(asset.meshletTriangles.size()));
        append32(payload, static_cast<std::uint32_t>(asset.indices.size()));
        append32(payload, static_cast<std::uint32_t>(asset.meshlets.size()));
        append32(payload, static_cast<std::uint32_t>(asset.lods.size()));
        append32(payload, static_cast<std::uint32_t>(asset.primitives.size()));
        append32(payload, static_cast<std::uint32_t>(asset.clusters.size()));
        append32(payload, static_cast<std::uint32_t>(asset.dagNodes.size()));
        append32(payload, static_cast<std::uint32_t>(asset.dagEdges.size()));
        append32(payload, static_cast<std::uint32_t>(boundaryCount));
        append32(payload, static_cast<std::uint32_t>(adjacencyCount));
        append32(payload,
            (METIS_VER_MAJOR << 16) | (METIS_VER_MINOR << 8) | METIS_VER_SUBMINOR);
        append32(payload, options.build.maxVertices);
        append32(payload, options.build.maxTriangles);
        appendFloat(payload, options.build.simplifyError);
        for (const float ratio : options.build.lodRatios)
            appendFloat(payload, ratio);
        append32(payload, options.build.metisSeed);
        append32(payload, options.build.metisUfactor);
        append32(payload, options.build.maxClusterVertices);
        append32(payload, options.build.maxClusterTriangles);
        append32(payload, options.build.maxDagDepth);
        appendFloat(payload, options.build.lodRefineThresholdPixels);
        appendFloat(payload, options.build.lodCoarsenThresholdPixels);
        append32(payload, dagChecksum(asset));
        append64(payload, layout.rawSize);
        append64(payload, layout.vertices.offset);
        append64(payload, layout.vertices.size);
        append64(payload, layout.meshletVertices.offset);
        append64(payload, layout.meshletVertices.size);
        append64(payload, layout.meshletTriangles.offset);
        append64(payload, layout.meshletTriangles.size);
        append64(payload, layout.indices.offset);
        append64(payload, layout.indices.size);
        append32(payload, layout.pageSize);
        append32(payload, layout.pageCount);
        append32(payload, static_cast<std::uint32_t>(layout.nodeDependencies.size()));
        append32(payload, static_cast<std::uint32_t>(layout.dependencyPageIndices.size()));
        append32(payload, static_cast<std::uint32_t>(layout.pageMeshlets.size()));

        for (const auto& meshlet : asset.meshlets)
        {
            append32(payload, meshlet.vertexOffset); append32(payload, meshlet.vertexCount);
            append32(payload, meshlet.triangleOffset); append32(payload, meshlet.triangleCount);
            append32(payload, meshlet.indexOffset); append32(payload, meshlet.indexCount);
            append32(payload, meshlet.primitiveIndex); append32(payload, meshlet.lodIndex);
            appendBytes(payload, &meshlet.sphere, sizeof(meshlet.sphere));
            appendBytes(payload, &meshlet.cone, sizeof(meshlet.cone));
            appendFloat(payload, meshlet.geometricError);
        }
        for (const auto& lod : asset.lods)
        {
            append32(payload, lod.primitiveIndex); append32(payload, lod.meshletOffset);
            append32(payload, lod.meshletCount); append32(payload, lod.indexOffset);
            append32(payload, lod.indexCount); appendFloat(payload, lod.geometricError);
            appendFloat(payload, lod.ratio);
        }
        for (const auto& primitive : asset.primitives)
        {
            append32(payload, primitive.vertexOffset); append32(payload, primitive.vertexCount);
            append32(payload, primitive.materialIndex);
            appendBytes(payload, &primitive.boundsMin, sizeof(primitive.boundsMin));
            appendBytes(payload, &primitive.boundsMax, sizeof(primitive.boundsMax));
        }
        for (const auto& cluster : asset.clusters)
        {
            append32(payload, cluster.meshletOffset); append32(payload, cluster.meshletCount);
            append32(payload, cluster.vertexOffset); append32(payload, cluster.vertexCount);
            append32(payload, cluster.triangleCount); append32(payload, cluster.lodDepth);
            append32(payload, cluster.primitiveIndex);
            appendBytes(payload, &cluster.sphere, sizeof(cluster.sphere));
            appendFloat(payload, cluster.geometricError);
            append32(payload, static_cast<std::uint32_t>(cluster.meshletIndices.size()));
            for (const auto value : cluster.meshletIndices) append32(payload, value);
            append32(payload, static_cast<std::uint32_t>(cluster.boundaryVertices.size()));
            for (const auto value : cluster.boundaryVertices) append32(payload, value);
            append32(payload, static_cast<std::uint32_t>(cluster.adjacentClusters.size()));
            for (const auto value : cluster.adjacentClusters) append32(payload, value);
        }
        for (const auto& node : asset.dagNodes)
        {
            append32(payload, node.clusterIndex); append32(payload, node.parentIndex);
            append32(payload, node.firstChild); append32(payload, node.childCount);
            append32(payload, node.lodDepth); append32(payload, node.flags);
            appendBytes(payload, &node.sphere, sizeof(node.sphere));
            appendFloat(payload, node.geometricError);
        }
        for (const auto& edge : asset.dagEdges)
        {
            append32(payload, edge.parent); append32(payload, edge.child);
        }
        for (const auto& range : layout.nodeDependencies)
        {
            append32(payload, range.offset); append32(payload, range.count);
        }
        for (const auto page : layout.dependencyPageIndices)
            append32(payload, page);
        for (const auto& address : layout.meshletAddresses)
        {
            append32(payload, address.pageIndex);
            append32(payload, address.vertexOffset);
            append32(payload, address.triangleOffset);
        }
        for (const auto& page : layout.pages)
        {
            append32(payload, page.meshletOffset); append32(payload, page.meshletCount);
            append32(payload, page.vertexCount); append32(payload, page.meshletVertexCount);
            append32(payload, page.triangleByteCount);
        }
        for (const auto& packed : layout.pageMeshlets)
        {
            append32(payload, packed.meshletIndex);
            append32(payload, packed.vertexOffset);
            append32(payload, packed.triangleOffset);
        }
        if (payload.size() > kMaxResidentMetadataBytes)
            return typedCacheError<std::vector<std::byte>>(
                "resident metadata exceeds the supported size");
        return Halcyon::Result<std::vector<std::byte>>::success(std::move(payload));
    }
    catch (const std::bad_alloc&)
    {
        return Halcyon::Result<std::vector<std::byte>>::failure(
            {Halcyon::ErrorCode::OutOfMemory,
                "unable to allocate resident cache metadata", "VirtualGeometryCache"});
    }
}

Halcyon::Result<void> deserializeResidentMetadata(std::span<const std::byte> bytes,
    VirtualGeometryCacheMetadata& result)
{
    Reader reader{bytes};
    std::uint32_t magic = 0u, version = 0u;
    std::uint32_t vertexCount = 0u, meshletVertexCount = 0u;
    std::uint32_t triangleByteCount = 0u, indexCount = 0u;
    std::uint32_t meshletCount = 0u, lodCount = 0u, primitiveCount = 0u;
    std::uint32_t clusterCount = 0u, dagNodeCount = 0u, dagEdgeCount = 0u;
    std::uint32_t boundaryCount = 0u, adjacencyCount = 0u, metisVersion = 0u;
    std::uint32_t storedDagChecksum = 0u, dependencyRangeCount = 0u;
    std::uint32_t dependencyPageCount = 0u, pageMeshletCount = 0u;
    auto& build = result.options.build;
    auto& layout = result.pageLayout;
    if (!reader.read32(magic) || !reader.read32(version) ||
        !reader.read32(vertexCount) || !reader.read32(meshletVertexCount) ||
        !reader.read32(triangleByteCount) || !reader.read32(indexCount) ||
        !reader.read32(meshletCount) || !reader.read32(lodCount) ||
        !reader.read32(primitiveCount) || !reader.read32(clusterCount) ||
        !reader.read32(dagNodeCount) || !reader.read32(dagEdgeCount) ||
        !reader.read32(boundaryCount) || !reader.read32(adjacencyCount) ||
        !reader.read32(metisVersion) || !reader.read32(build.maxVertices) ||
        !reader.read32(build.maxTriangles) || !reader.readFloat(build.simplifyError))
        return cacheVoidError("resident metadata header is truncated");
    for (float& ratio : build.lodRatios)
        if (!reader.readFloat(ratio))
            return cacheVoidError("resident LOD ratio table is truncated");
    if (!reader.read32(build.metisSeed) ||
        !reader.read32(build.metisUfactor) ||
        !reader.read32(build.maxClusterVertices) ||
        !reader.read32(build.maxClusterTriangles) ||
        !reader.read32(build.maxDagDepth) ||
        !reader.readFloat(build.lodRefineThresholdPixels) ||
        !reader.readFloat(build.lodCoarsenThresholdPixels) ||
        !reader.read32(storedDagChecksum) || !reader.read64(layout.rawSize) ||
        !reader.read64(layout.vertices.offset) || !reader.read64(layout.vertices.size) ||
        !reader.read64(layout.meshletVertices.offset) ||
        !reader.read64(layout.meshletVertices.size) ||
        !reader.read64(layout.meshletTriangles.offset) ||
        !reader.read64(layout.meshletTriangles.size) ||
        !reader.read64(layout.indices.offset) || !reader.read64(layout.indices.size) ||
        !reader.read32(layout.pageSize) || !reader.read32(layout.pageCount) ||
        !reader.read32(dependencyRangeCount) || !reader.read32(dependencyPageCount) ||
        !reader.read32(pageMeshletCount))
        return cacheVoidError("resident metadata header is truncated");
    if (magic != kResidentMetadataMagic || version != kResidentMetadataVersion)
        return cacheVoidError("resident metadata version is incompatible");
    if (metisVersion !=
            ((METIS_VER_MAJOR << 16) | (METIS_VER_MINOR << 8) | METIS_VER_SUBMINOR) ||
        validateBuildOptions(build).has_value())
        return cacheVoidError("resident METIS metadata or build options are invalid");
    if (vertexCount > 100000000u || meshletVertexCount > 100000000u ||
        triangleByteCount > 300000000u || indexCount > 300000000u ||
        meshletCount > kMaxVisibilityMeshlets || lodCount > 1000000u ||
        primitiveCount > 1000000u || clusterCount > 1000000u ||
        dagNodeCount > 2000000u || dagEdgeCount > 4000000u ||
        boundaryCount > 100000000u || adjacencyCount > 100000000u ||
        dependencyRangeCount != dagNodeCount || dependencyPageCount > 100000000u ||
        pageMeshletCount > 100000000u)
        return cacheVoidError("resident metadata counts are unreasonable");

    const auto validateStream = [&](VirtualGeometryStreamRange range,
                                    std::uint64_t count, std::uint64_t stride)
    {
        std::uint64_t size = 0u;
        return tableByteSize(count, stride, size) && range.size == size;
    };
    if (layout.pageSize != result.options.pageSize || layout.pageSize == 0u ||
        layout.pageCount == 0u ||
        layout.rawSize != static_cast<std::uint64_t>(layout.pageCount) * layout.pageSize ||
        !validateStream(layout.vertices, vertexCount, sizeof(StaticSceneVertex)) ||
        !validateStream(layout.meshletVertices, meshletVertexCount, sizeof(std::uint32_t)) ||
        !validateStream(layout.meshletTriangles, triangleByteCount, sizeof(std::uint8_t)) ||
        !validateStream(layout.indices, indexCount, sizeof(std::uint32_t)) ||
        layout.vertices.offset != 0u ||
        layout.meshletVertices.offset != layout.vertices.size ||
        layout.meshletTriangles.offset !=
            layout.vertices.size + layout.meshletVertices.size ||
        layout.indices.offset != layout.vertices.size + layout.meshletVertices.size +
            layout.meshletTriangles.size)
        return cacheVoidError("resident virtual-address layout is invalid");

    auto& asset = result.residentAsset;
    try
    {
        asset.meshlets.resize(meshletCount);
        asset.lods.resize(lodCount);
        asset.primitives.resize(primitiveCount);
        asset.clusters.resize(clusterCount);
        asset.dagNodes.resize(dagNodeCount);
        asset.dagEdges.resize(dagEdgeCount);
        layout.nodeDependencies.resize(dependencyRangeCount);
        layout.dependencyPageIndices.resize(dependencyPageCount);
        layout.meshletAddresses.resize(meshletCount);
        layout.pages.resize(layout.pageCount);
        layout.pageMeshlets.resize(pageMeshletCount);
    }
    catch (const std::bad_alloc&)
    {
        return Halcyon::Result<void>::failure({Halcyon::ErrorCode::OutOfMemory,
            "unable to allocate resident cache tables", "VirtualGeometryCache"});
    }
    for (auto& meshlet : asset.meshlets)
        if (!reader.read32(meshlet.vertexOffset) || !reader.read32(meshlet.vertexCount) ||
            !reader.read32(meshlet.triangleOffset) || !reader.read32(meshlet.triangleCount) ||
            !reader.read32(meshlet.indexOffset) || !reader.read32(meshlet.indexCount) ||
            !reader.read32(meshlet.primitiveIndex) || !reader.read32(meshlet.lodIndex) ||
            !reader.readBytes(&meshlet.sphere, sizeof(meshlet.sphere)) ||
            !reader.readBytes(&meshlet.cone, sizeof(meshlet.cone)) ||
            !reader.readFloat(meshlet.geometricError))
            return cacheVoidError("resident Meshlet table is truncated");
    for (auto& lod : asset.lods)
        if (!reader.read32(lod.primitiveIndex) || !reader.read32(lod.meshletOffset) ||
            !reader.read32(lod.meshletCount) || !reader.read32(lod.indexOffset) ||
            !reader.read32(lod.indexCount) || !reader.readFloat(lod.geometricError) ||
            !reader.readFloat(lod.ratio))
            return cacheVoidError("resident LOD table is truncated");
    for (auto& primitive : asset.primitives)
        if (!reader.read32(primitive.vertexOffset) || !reader.read32(primitive.vertexCount) ||
            !reader.read32(primitive.materialIndex) ||
            !reader.readBytes(&primitive.boundsMin, sizeof(primitive.boundsMin)) ||
            !reader.readBytes(&primitive.boundsMax, sizeof(primitive.boundsMax)))
            return cacheVoidError("resident primitive table is truncated");
    std::uint64_t observedBoundaryCount = 0u;
    std::uint64_t observedAdjacencyCount = 0u;
    for (auto& cluster : asset.clusters)
    {
        std::uint32_t meshletIndexCount = 0u, clusterBoundaryCount = 0u;
        std::uint32_t clusterAdjacencyCount = 0u;
        if (!reader.read32(cluster.meshletOffset) || !reader.read32(cluster.meshletCount) ||
            !reader.read32(cluster.vertexOffset) || !reader.read32(cluster.vertexCount) ||
            !reader.read32(cluster.triangleCount) || !reader.read32(cluster.lodDepth) ||
            !reader.read32(cluster.primitiveIndex) ||
            !reader.readBytes(&cluster.sphere, sizeof(cluster.sphere)) ||
            !reader.readFloat(cluster.geometricError) ||
            !reader.read32(meshletIndexCount) || meshletIndexCount != cluster.meshletCount ||
            meshletIndexCount > meshletCount)
            return cacheVoidError("resident Cluster table is invalid");
        cluster.meshletIndices.resize(meshletIndexCount);
        for (auto& value : cluster.meshletIndices)
            if (!reader.read32(value))
                return cacheVoidError("resident Cluster Meshlet list is truncated");
        if (!reader.read32(clusterBoundaryCount) ||
            observedBoundaryCount + clusterBoundaryCount > boundaryCount)
            return cacheVoidError("resident boundary table is invalid");
        cluster.boundaryVertices.resize(clusterBoundaryCount);
        for (auto& value : cluster.boundaryVertices)
            if (!reader.read32(value))
                return cacheVoidError("resident boundary table is truncated");
        observedBoundaryCount += clusterBoundaryCount;
        if (!reader.read32(clusterAdjacencyCount) ||
            observedAdjacencyCount + clusterAdjacencyCount > adjacencyCount)
            return cacheVoidError("resident adjacency table is invalid");
        cluster.adjacentClusters.resize(clusterAdjacencyCount);
        for (auto& value : cluster.adjacentClusters)
            if (!reader.read32(value))
                return cacheVoidError("resident adjacency table is truncated");
        observedAdjacencyCount += clusterAdjacencyCount;
    }
    for (auto& node : asset.dagNodes)
        if (!reader.read32(node.clusterIndex) || !reader.read32(node.parentIndex) ||
            !reader.read32(node.firstChild) || !reader.read32(node.childCount) ||
            !reader.read32(node.lodDepth) || !reader.read32(node.flags) ||
            !reader.readBytes(&node.sphere, sizeof(node.sphere)) ||
            !reader.readFloat(node.geometricError))
            return cacheVoidError("resident DAG node table is truncated");
    for (auto& edge : asset.dagEdges)
        if (!reader.read32(edge.parent) || !reader.read32(edge.child))
            return cacheVoidError("resident DAG edge table is truncated");
    for (auto& range : layout.nodeDependencies)
        if (!reader.read32(range.offset) || !reader.read32(range.count))
            return cacheVoidError("resident page dependency ranges are truncated");
    for (auto& page : layout.dependencyPageIndices)
        if (!reader.read32(page))
            return cacheVoidError("resident page dependency table is truncated");
    for (auto& address : layout.meshletAddresses)
        if (!reader.read32(address.pageIndex) || !reader.read32(address.vertexOffset) ||
            !reader.read32(address.triangleOffset))
            return cacheVoidError("resident Meshlet page addresses are truncated");
    for (auto& page : layout.pages)
        if (!reader.read32(page.meshletOffset) || !reader.read32(page.meshletCount) ||
            !reader.read32(page.vertexCount) || !reader.read32(page.meshletVertexCount) ||
            !reader.read32(page.triangleByteCount))
            return cacheVoidError("resident page descriptors are truncated");
    for (auto& packed : layout.pageMeshlets)
        if (!reader.read32(packed.meshletIndex) || !reader.read32(packed.vertexOffset) ||
            !reader.read32(packed.triangleOffset))
            return cacheVoidError("resident page Meshlet table is truncated");
    if (reader.offset != bytes.size() || observedBoundaryCount != boundaryCount ||
        observedAdjacencyCount != adjacencyCount || !validateVirtualGeometryDag(asset) ||
        storedDagChecksum != dagChecksum(asset))
        return cacheVoidError("resident metadata topology validation failed");
    for (const auto& range : layout.nodeDependencies)
    {
        if (static_cast<std::size_t>(range.offset) > layout.dependencyPageIndices.size() ||
            static_cast<std::size_t>(range.count) >
                layout.dependencyPageIndices.size() - range.offset)
            return cacheVoidError("resident page dependency range is invalid");
        for (std::uint32_t i = 0u; i < range.count; ++i)
        {
            const auto page = layout.dependencyPageIndices[range.offset + i];
            if (page >= layout.pageCount ||
                (i != 0u && layout.dependencyPageIndices[range.offset + i - 1u] >= page))
                return cacheVoidError("resident page dependencies are not sorted and valid");
        }
    }
    if (layout.meshletAddresses.size() != meshletCount ||
        layout.pages.size() != layout.pageCount)
        return cacheVoidError("resident page address tables are incomplete");
    for (const auto& address : layout.meshletAddresses)
        if (address.pageIndex >= layout.pageCount)
            return cacheVoidError("resident Meshlet page address is out of range");
    for (const auto& page : layout.pages)
        if (static_cast<std::size_t>(page.meshletOffset) > layout.pageMeshlets.size() ||
            static_cast<std::size_t>(page.meshletCount) >
                layout.pageMeshlets.size() - page.meshletOffset)
            return cacheVoidError("resident page Meshlet range is invalid");
    return Halcyon::Result<void>::success();
}

} // namespace

Halcyon::Result<void> writeVirtualGeometryLegacyPayloadCache(const std::filesystem::path& path,
    const VirtualGeometryAsset& asset, const Sha256Digest& sourceHash,
    const VirtualGeometryCacheOptions& options)
{
    if (options.compressionLevel > static_cast<std::uint32_t>(ZSTD_maxCLevel()))
        return Halcyon::Err({Halcyon::ErrorCode::InvalidArgument,
            "cache options are invalid", path.string()});
    if (options.pageSize < 4096u || options.pageSize > 1024u * 1024u ||
        (options.pageSize & (options.pageSize - 1u)) != 0u)
        return Halcyon::Err({Halcyon::ErrorCode::InvalidArgument,
            "cache page size must be a power of two between 4 KiB and 1 MiB",
            path.string()});
    if (const auto invalid = validateBuildOptions(options.build))
        return Halcyon::Err({Halcyon::ErrorCode::InvalidArgument,
            std::string(*invalid), path.string()});
    if (const auto invalid = validateAsset(asset, options.build))
        return Halcyon::Err({Halcyon::ErrorCode::InvalidArgument,
            std::string(*invalid), path.string()});
    try
    {
    const auto fits32 = [](std::size_t value) noexcept
    {
        return value <= std::numeric_limits<std::uint32_t>::max();
    };
    if (!fits32(asset.vertices.size()) || !fits32(asset.meshletVertices.size()) ||
        !fits32(asset.meshletTriangles.size()) || !fits32(asset.indices.size()) ||
        !fits32(asset.lods.size()) || !fits32(asset.primitives.size()) ||
        !fits32(asset.clusters.size()) || !fits32(asset.dagNodes.size()) ||
        !fits32(asset.dagEdges.size()))
        return Halcyon::Err({Halcyon::ErrorCode::InvalidArgument,
            "asset table exceeds the cache integer range", path.string()});
    std::vector<std::byte> payload;
    append32(payload, static_cast<std::uint32_t>(asset.vertices.size()));
    append32(payload, static_cast<std::uint32_t>(asset.meshletVertices.size()));
    append32(payload, static_cast<std::uint32_t>(asset.meshletTriangles.size()));
    append32(payload, static_cast<std::uint32_t>(asset.indices.size()));
    append32(payload, static_cast<std::uint32_t>(asset.meshlets.size()));
    append32(payload, static_cast<std::uint32_t>(asset.lods.size()));
    append32(payload, static_cast<std::uint32_t>(asset.primitives.size()));
    append32(payload, static_cast<std::uint32_t>(asset.clusters.size()));
    append32(payload, static_cast<std::uint32_t>(asset.dagNodes.size()));
    append32(payload, static_cast<std::uint32_t>(asset.dagEdges.size()));
    std::size_t boundaryCount = 0;
    std::size_t adjacencyCount = 0;
    for (const auto& cluster : asset.clusters)
    {
        boundaryCount += cluster.boundaryVertices.size();
        adjacencyCount += cluster.adjacentClusters.size();
    }
    if (!fits32(boundaryCount) || !fits32(adjacencyCount))
        return Halcyon::Err({Halcyon::ErrorCode::InvalidArgument,
            "cluster auxiliary tables exceed the cache integer range", path.string()});
    append32(payload, static_cast<std::uint32_t>(boundaryCount));
    append32(payload, static_cast<std::uint32_t>(adjacencyCount));
    append32(payload, (METIS_VER_MAJOR << 16) | (METIS_VER_MINOR << 8) | METIS_VER_SUBMINOR);
    append32(payload, options.build.metisSeed);
    append32(payload, options.build.metisUfactor);
    append32(payload, options.build.maxClusterVertices);
    append32(payload, options.build.maxClusterTriangles);
    append32(payload, options.build.maxDagDepth);
    appendFloat(payload, options.build.lodRefineThresholdPixels);
    appendFloat(payload, options.build.lodCoarsenThresholdPixels);
    append32(payload, dagChecksum(asset));
    for (const auto& vertex : asset.vertices)
    {
        appendBytes(payload, &vertex.position, sizeof(vertex.position));
        appendBytes(payload, &vertex.normal, sizeof(vertex.normal));
        appendBytes(payload, &vertex.uv, sizeof(vertex.uv));
        appendBytes(payload, &vertex.tangent, sizeof(vertex.tangent));
    }
    for (const auto value : asset.meshletVertices) append32(payload, value);
    appendBytes(payload, asset.meshletTriangles.data(), asset.meshletTriangles.size());
    for (const auto value : asset.indices) append32(payload, value);
    for (const auto& meshlet : asset.meshlets)
    {
        append32(payload, meshlet.vertexOffset); append32(payload, meshlet.vertexCount);
        append32(payload, meshlet.triangleOffset); append32(payload, meshlet.triangleCount);
        append32(payload, meshlet.indexOffset); append32(payload, meshlet.indexCount);
        append32(payload, meshlet.primitiveIndex); append32(payload, meshlet.lodIndex);
        appendBytes(payload, &meshlet.sphere, sizeof(meshlet.sphere));
        appendBytes(payload, &meshlet.cone, sizeof(meshlet.cone)); appendFloat(payload, meshlet.geometricError);
    }
    for (const auto& lod : asset.lods)
    {
        append32(payload, lod.primitiveIndex); append32(payload, lod.meshletOffset); append32(payload, lod.meshletCount);
        append32(payload, lod.indexOffset); append32(payload, lod.indexCount); appendFloat(payload, lod.geometricError); appendFloat(payload, lod.ratio);
    }
    for (const auto& primitive : asset.primitives)
    {
        append32(payload, primitive.vertexOffset); append32(payload, primitive.vertexCount); append32(payload, primitive.materialIndex);
        appendBytes(payload, &primitive.boundsMin, sizeof(primitive.boundsMin)); appendBytes(payload, &primitive.boundsMax, sizeof(primitive.boundsMax));
    }
    for (const auto& cluster : asset.clusters)
    {
        append32(payload, cluster.meshletOffset); append32(payload, cluster.meshletCount);
        append32(payload, cluster.vertexOffset); append32(payload, cluster.vertexCount);
        append32(payload, cluster.triangleCount); append32(payload, cluster.lodDepth);
        append32(payload, cluster.primitiveIndex);
        appendBytes(payload, &cluster.sphere, sizeof(cluster.sphere)); appendFloat(payload, cluster.geometricError);
        append32(payload, static_cast<std::uint32_t>(cluster.meshletIndices.size()));
        for (const auto value : cluster.meshletIndices) append32(payload, value);
        append32(payload, static_cast<std::uint32_t>(cluster.boundaryVertices.size()));
        for (const auto value : cluster.boundaryVertices) append32(payload, value);
        append32(payload, static_cast<std::uint32_t>(cluster.adjacentClusters.size()));
        for (const auto value : cluster.adjacentClusters) append32(payload, value);
    }
    for (const auto& node : asset.dagNodes)
    {
        append32(payload, node.clusterIndex); append32(payload, node.parentIndex);
        append32(payload, node.firstChild); append32(payload, node.childCount);
        append32(payload, node.lodDepth); append32(payload, node.flags);
        appendBytes(payload, &node.sphere, sizeof(node.sphere)); appendFloat(payload, node.geometricError);
    }
    for (const auto& edge : asset.dagEdges)
    {
        append32(payload, edge.parent); append32(payload, edge.child);
    }
    if (payload.size() > kMaxCachePayloadBytes)
        return Halcyon::Err({Halcyon::ErrorCode::InvalidArgument,
            "cache payload exceeds the supported size", path.string()});
    struct CompressedPage
    {
        VirtualGeometryCachePageEntry entry;
        std::vector<std::byte> bytes;
    };
    const std::size_t pageCount =
        (payload.size() + options.pageSize - 1u) / options.pageSize;
    if (pageCount == 0u || pageCount > kMaxCachePageCount)
        return Halcyon::Err({Halcyon::ErrorCode::InvalidArgument,
            "cache page count exceeds the supported range", path.string()});
    std::vector<CompressedPage> pages;
    pages.reserve(pageCount);
    ZSTD_CCtx* context = ZSTD_createCCtx();
    if (!context) return Halcyon::Err({Halcyon::ErrorCode::Io, "unable to create zstd context", path.string()});
    const std::size_t parameterResult = ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel, static_cast<int>(options.compressionLevel));
    const std::size_t checksumResult = ZSTD_CCtx_setParameter(context, ZSTD_c_checksumFlag, 1);
    if (ZSTD_isError(parameterResult) || ZSTD_isError(checksumResult))
    {
        ZSTD_freeCCtx(context);
        return Halcyon::Err({Halcyon::ErrorCode::Io,
            "zstd page compression parameters are unsupported", path.string()});
    }
    std::uint64_t rawOffset = 0u;
    std::uint64_t fileOffset = kHeaderSize + pageCount * kPageDirectoryEntrySize;
    for (std::size_t pageIndex = 0; pageIndex < pageCount; ++pageIndex)
    {
        const std::size_t rawPageSize = std::min<std::size_t>(options.pageSize,
            payload.size() - static_cast<std::size_t>(rawOffset));
        const std::span<const std::byte> rawPage(
            payload.data() + static_cast<std::size_t>(rawOffset), rawPageSize);
        CompressedPage page;
        page.bytes.resize(ZSTD_compressBound(rawPageSize));
        const std::size_t compressedSize = ZSTD_compress2(context, page.bytes.data(),
            page.bytes.size(), rawPage.data(), rawPage.size());
        if (ZSTD_isError(compressedSize) ||
            compressedSize > std::numeric_limits<std::uint32_t>::max())
        {
            ZSTD_freeCCtx(context);
            return Halcyon::Err({Halcyon::ErrorCode::Io,
                ZSTD_isError(compressedSize) ? ZSTD_getErrorName(compressedSize) :
                    "compressed cache page exceeds the integer range",
                "zstd page compression"});
        }
        page.bytes.resize(compressedSize);
        page.entry.fileOffset = fileOffset;
        page.entry.rawOffset = rawOffset;
        page.entry.compressedSize = static_cast<std::uint32_t>(compressedSize);
        page.entry.rawSize = static_cast<std::uint32_t>(rawPageSize);
        page.entry.crc32 = crc32(rawPage);
        page.entry.flags = pageIndex == 0u ? VirtualGeometryCachePagePinnedRoot :
            VirtualGeometryCachePageNone;
        pages.push_back(std::move(page));
        rawOffset += rawPageSize;
        fileOffset += compressedSize;
    }
    ZSTD_freeCCtx(context);
    std::vector<std::byte> header;
    header.insert(header.end(), reinterpret_cast<const std::byte*>(kMagic.data()), reinterpret_cast<const std::byte*>(kMagic.data() + kMagic.size()));
    append32(header, kVirtualGeometryCacheVersion); append32(header, kEndian); appendBytes(header, sourceHash.data(), sourceHash.size());
    append32(header, options.build.maxVertices); append32(header, options.build.maxTriangles); append32(header, options.compressionLevel);
    append32(header, MESHOPTIMIZER_VERSION); appendFloat(header, options.build.simplifyError);
    for (float ratio : options.build.lodRatios) appendFloat(header, ratio);
    append32(header, options.pageSize);
    append32(header, static_cast<std::uint32_t>(pages.size()));
    append32(header, 1u);
    append32(header, 0u);
    append64(header, kHeaderSize);
    append64(header, pages.size() * kPageDirectoryEntrySize);
    append64(header, kHeaderSize + pages.size() * kPageDirectoryEntrySize);
    append64(header, payload.size());
    if (header.size() != kHeaderSize)
        return Halcyon::Err({Halcyon::ErrorCode::InvalidState,
            "cache header ABI size mismatch", path.string()});
    const auto temporary = path.string() + ".tmp"; std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) return Halcyon::Err({Halcyon::ErrorCode::Io, "unable to create cache", path.string()});
    stream.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    for (const auto& page : pages)
    {
        std::vector<std::byte> entry;
        entry.reserve(kPageDirectoryEntrySize);
        append64(entry, page.entry.fileOffset);
        append64(entry, page.entry.rawOffset);
        append32(entry, page.entry.compressedSize);
        append32(entry, page.entry.rawSize);
        append32(entry, page.entry.crc32);
        append32(entry, page.entry.flags);
        stream.write(reinterpret_cast<const char*>(entry.data()),
            static_cast<std::streamsize>(entry.size()));
    }
    for (const auto& page : pages)
        stream.write(reinterpret_cast<const char*>(page.bytes.data()),
            static_cast<std::streamsize>(page.bytes.size()));
    stream.close();
    if (!stream) return Halcyon::Err({Halcyon::ErrorCode::Io, "unable to write cache", path.string()});
    std::error_code error; std::filesystem::rename(temporary, path, error);
    if (error) { std::filesystem::remove(path, error); std::filesystem::rename(temporary, path, error); }
    if (error) { std::filesystem::remove(temporary, error); return Halcyon::Err({Halcyon::ErrorCode::Io, "unable to publish cache", path.string()}); }
    return Halcyon::Ok();
    }
    catch (const std::bad_alloc&)
    {
        return Halcyon::Err({Halcyon::ErrorCode::OutOfMemory,
            "unable to allocate cache payload", path.string()});
    }
}

Halcyon::Result<VirtualGeometryCacheMetadata> readVirtualGeometryLegacyPayloadMetadata(
    const std::filesystem::path& path, const Sha256Digest* expectedSourceHash,
    const VirtualGeometryCacheOptions* expectedOptions)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        return typedCacheError<VirtualGeometryCacheMetadata>("cache does not exist");
    const auto size = stream.tellg(); if (size < 0 || static_cast<std::uint64_t>(size) < kHeaderSize ||
        static_cast<std::uintmax_t>(size) > std::numeric_limits<std::size_t>::max())
        return typedCacheError<VirtualGeometryCacheMetadata>("cache is truncated");
    std::array<std::byte, kHeaderSize> fileHeader{};
    stream.seekg(0); stream.read(reinterpret_cast<char*>(fileHeader.data()),
        static_cast<std::streamsize>(fileHeader.size()));
    if (!stream)
        return typedCacheError<VirtualGeometryCacheMetadata>("unable to read cache");
    if (std::memcmp(fileHeader.data(), kMagic.data(), kMagic.size()) != 0)
        return typedCacheError<VirtualGeometryCacheMetadata>("cache magic mismatch");
    Reader header{fileHeader};
    header.offset = 8;
    std::uint32_t version = 0, endian = 0, maxVertices = 0, maxTriangles = 0;
    std::uint32_t level = 0, meshoptVersion = 0, pageSize = 0, pageCount = 0;
    std::uint32_t rootPageCount = 0, reserved = 0;
    float simplifyError = 0.0f;
    std::array<float, 3> lodRatios{};
    std::uint64_t directoryOffset = 0, directorySize = 0, payloadOffset = 0;
    std::uint64_t rawSize = 0;
    Sha256Digest source{};
    if (!header.read32(version) || !header.read32(endian) ||
        !header.readBytes(source.data(), source.size()) || !header.read32(maxVertices) ||
        !header.read32(maxTriangles) || !header.read32(level) ||
        !header.read32(meshoptVersion) || !header.readFloat(simplifyError) ||
        !header.readFloat(lodRatios[0]) || !header.readFloat(lodRatios[1]) ||
        !header.readFloat(lodRatios[2]) || !header.read32(pageSize) ||
        !header.read32(pageCount) || !header.read32(rootPageCount) ||
        !header.read32(reserved) || !header.read64(directoryOffset) ||
        !header.read64(directorySize) || !header.read64(payloadOffset) ||
        !header.read64(rawSize))
        return typedCacheError<VirtualGeometryCacheMetadata>("cache header is truncated");
    const bool invalidRatio = std::any_of(lodRatios.begin(), lodRatios.end(),
        [](float ratio) { return !std::isfinite(ratio) || ratio < 0.0f || ratio > 1.0f; });
    VirtualGeometryBuildOptions serializedBuild{};
    serializedBuild.maxVertices = maxVertices;
    serializedBuild.maxTriangles = maxTriangles;
    serializedBuild.simplifyError = simplifyError;
    for (std::size_t i = 0u; i < lodRatios.size(); ++i)
        serializedBuild.lodRatios[i] = lodRatios[i];
    if (version != kVirtualGeometryCacheVersion)
        return typedCacheError<VirtualGeometryCacheMetadata>("cache version " +
            std::to_string(version) + " is incompatible with v5; re-run HalcyonCooker");
    const bool invalidPageSize = pageSize < 4096u || pageSize > 1024u * 1024u ||
        (pageSize & (pageSize - 1u)) != 0u;
    const std::uint64_t expectedDirectorySize =
        static_cast<std::uint64_t>(pageCount) * kPageDirectoryEntrySize;
    if (endian != kEndian || meshoptVersion != MESHOPTIMIZER_VERSION || invalidRatio ||
        validateBuildOptions(serializedBuild).has_value() ||
        level > static_cast<std::uint32_t>(ZSTD_maxCLevel()) ||
        invalidPageSize || pageCount == 0u || pageCount > kMaxCachePageCount ||
        rootPageCount == 0u || rootPageCount > pageCount || reserved != 0u ||
        directoryOffset != kHeaderSize || directorySize != expectedDirectorySize ||
        payloadOffset != directoryOffset + directorySize || rawSize == 0u ||
        rawSize > kMaxCachePayloadBytes || rawSize > std::numeric_limits<std::size_t>::max() ||
        payloadOffset > static_cast<std::uint64_t>(size))
        return typedCacheError<VirtualGeometryCacheMetadata>("cache header is invalid");
    if (expectedSourceHash && *expectedSourceHash != source)
        return typedCacheError<VirtualGeometryCacheMetadata>("cache source hash mismatch");
    if (expectedOptions != nullptr &&
        (expectedOptions->build.maxVertices != maxVertices ||
            expectedOptions->build.maxTriangles != maxTriangles ||
            expectedOptions->build.simplifyError != simplifyError ||
            !std::equal(lodRatios.begin(), lodRatios.end(),
                expectedOptions->build.lodRatios.begin()) ||
            expectedOptions->compressionLevel != level ||
            expectedOptions->pageSize != pageSize))
        return typedCacheError<VirtualGeometryCacheMetadata>(
            "cache cooker parameters mismatch");

    std::vector<std::byte> directory;
    try { directory.resize(static_cast<std::size_t>(directorySize)); }
    catch (const std::bad_alloc&)
    {
        return Halcyon::Result<VirtualGeometryCacheMetadata>::failure(
            {Halcyon::ErrorCode::OutOfMemory,
                "unable to allocate cache page directory", "VirtualGeometryCache"});
    }
    stream.seekg(static_cast<std::streamoff>(directoryOffset));
    stream.read(reinterpret_cast<char*>(directory.data()),
        static_cast<std::streamsize>(directory.size()));
    if (!stream)
        return typedCacheError<VirtualGeometryCacheMetadata>(
            "cache page directory is truncated");

    VirtualGeometryCacheMetadata result;
    result.sourceHash = source;
    result.options.build = serializedBuild;
    result.options.compressionLevel = level;
    result.options.pageSize = pageSize;
    result.rawPayloadSize = rawSize;
    result.rootPageCount = rootPageCount;
    result.pages.resize(pageCount);
    Reader pageReader{directory};
    std::uint64_t expectedRawOffset = 0u;
    std::uint64_t expectedFileOffset = payloadOffset;
    std::uint32_t observedRootPages = 0u;
    for (std::uint32_t pageIndex = 0; pageIndex < pageCount; ++pageIndex)
    {
        auto& page = result.pages[pageIndex];
        if (!pageReader.read64(page.fileOffset) || !pageReader.read64(page.rawOffset) ||
            !pageReader.read32(page.compressedSize) || !pageReader.read32(page.rawSize) ||
            !pageReader.read32(page.crc32) || !pageReader.read32(page.flags))
            return typedCacheError<VirtualGeometryCacheMetadata>(
                "cache page directory is truncated");
        if (page.fileOffset != expectedFileOffset || page.rawOffset != expectedRawOffset ||
            page.compressedSize < 5u || page.rawSize == 0u || page.rawSize > pageSize ||
            (pageIndex + 1u != pageCount && page.rawSize != pageSize) ||
            (page.flags & ~VirtualGeometryCachePagePinnedRoot) != 0u ||
            page.fileOffset > static_cast<std::uint64_t>(size) ||
            page.compressedSize > static_cast<std::uint64_t>(size) - page.fileOffset)
            return typedCacheError<VirtualGeometryCacheMetadata>(
                "cache page directory entry is invalid");
        expectedRawOffset += page.rawSize;
        expectedFileOffset += page.compressedSize;
        if ((page.flags & VirtualGeometryCachePagePinnedRoot) != 0u)
        {
            ++observedRootPages;
            result.rootPageIndices.push_back(pageIndex);
        }
    }
    if (pageReader.offset != directory.size() || expectedRawOffset != rawSize ||
        expectedFileOffset != static_cast<std::uint64_t>(size) ||
        observedRootPages != rootPageCount)
        return typedCacheError<VirtualGeometryCacheMetadata>(
            "cache page directory ranges do not cover the file");
    return Halcyon::Result<VirtualGeometryCacheMetadata>::success(std::move(result));
}

Halcyon::Result<std::vector<std::byte>> readVirtualGeometryCachePage(
    const std::filesystem::path& path, const VirtualGeometryCacheMetadata& metadata,
    std::uint32_t pageIndex)
{
    if (pageIndex >= metadata.pages.size())
        return typedCacheError<std::vector<std::byte>>("cache page index is out of range");
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return typedCacheError<std::vector<std::byte>>("cache does not exist");
    return readCompressedPage(stream, metadata.pages[pageIndex]);
}

Halcyon::Result<VirtualGeometryAsset> readVirtualGeometryLegacyPayloadCache(const std::filesystem::path& path,
    const Sha256Digest* expectedSourceHash, VirtualGeometryCacheOptions* metadata,
    const VirtualGeometryCacheOptions* expectedOptions)
{
    const auto cacheMetadata = readVirtualGeometryLegacyPayloadMetadata(
        path, expectedSourceHash, expectedOptions);
    if (!cacheMetadata)
        return Halcyon::Result<VirtualGeometryAsset>::failure(cacheMetadata.error());
    const std::uint32_t maxVertices = cacheMetadata->options.build.maxVertices;
    const std::uint32_t maxTriangles = cacheMetadata->options.build.maxTriangles;
    const std::uint32_t level = cacheMetadata->options.compressionLevel;
    const float simplifyError = cacheMetadata->options.build.simplifyError;
    const auto& lodRatios = cacheMetadata->options.build.lodRatios;
    VirtualGeometryBuildOptions serializedBuild = cacheMetadata->options.build;
    std::uint32_t metisSeed = 0u;
    std::uint32_t metisUfactor = 0u;
    std::uint32_t maxClusterVertices = 0u;
    std::uint32_t maxClusterTriangles = 0u;
    std::uint32_t maxDagDepth = 0u;
    float refineThreshold = 0.0f;
    float coarsenThreshold = 0.0f;
    std::vector<std::byte> payload;
    try { payload.resize(static_cast<std::size_t>(cacheMetadata->rawPayloadSize)); }
    catch (const std::bad_alloc&) { return cacheAllocationError(); }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return cacheError("cache does not exist");
    for (const auto& entry : cacheMetadata->pages)
    {
        auto page = readCompressedPage(stream, entry);
        if (!page)
            return Halcyon::Result<VirtualGeometryAsset>::failure(page.error());
        std::memcpy(payload.data() + static_cast<std::size_t>(entry.rawOffset),
            page->data(), page->size());
    }
    Reader reader{payload}; std::uint32_t vertexCount = 0, meshletVertexCount = 0, triangleByteCount = 0, indexCount = 0, meshletCount = 0, lodCount = 0, primitiveCount = 0, clusterCount = 0, dagNodeCount = 0, dagEdgeCount = 0, boundaryCount = 0, adjacencyCount = 0, metisVersion = 0, storedDagChecksum = 0;
    if (!reader.read32(vertexCount) || !reader.read32(meshletVertexCount) || !reader.read32(triangleByteCount) || !reader.read32(indexCount) || !reader.read32(meshletCount) || !reader.read32(lodCount) || !reader.read32(primitiveCount) || !reader.read32(clusterCount) || !reader.read32(dagNodeCount) || !reader.read32(dagEdgeCount) || !reader.read32(boundaryCount) || !reader.read32(adjacencyCount) || !reader.read32(metisVersion) || !reader.read32(metisSeed) || !reader.read32(metisUfactor) || !reader.read32(maxClusterVertices) || !reader.read32(maxClusterTriangles) || !reader.read32(maxDagDepth) || !reader.readFloat(refineThreshold) || !reader.readFloat(coarsenThreshold) || !reader.read32(storedDagChecksum)) return cacheError("cache payload header is truncated");
    serializedBuild.metisSeed = metisSeed;
    serializedBuild.metisUfactor = metisUfactor;
    serializedBuild.maxClusterVertices = maxClusterVertices;
    serializedBuild.maxClusterTriangles = maxClusterTriangles;
    serializedBuild.maxDagDepth = maxDagDepth;
    serializedBuild.lodRefineThresholdPixels = refineThreshold;
    serializedBuild.lodCoarsenThresholdPixels = coarsenThreshold;
    if (validateBuildOptions(serializedBuild).has_value())
        return cacheError("cache M6 build options are invalid");
    if (expectedOptions != nullptr &&
        (expectedOptions->build.metisSeed != metisSeed ||
            expectedOptions->build.metisUfactor != metisUfactor ||
            expectedOptions->build.maxClusterVertices != maxClusterVertices ||
            expectedOptions->build.maxClusterTriangles != maxClusterTriangles ||
            expectedOptions->build.maxDagDepth != maxDagDepth ||
            expectedOptions->build.lodRefineThresholdPixels != refineThreshold ||
            expectedOptions->build.lodCoarsenThresholdPixels != coarsenThreshold))
        return cacheError("cache M6 cooker parameters mismatch");
    if (vertexCount > 100000000u || meshletVertexCount > 100000000u || triangleByteCount > 300000000u || indexCount > 300000000u || meshletCount > kMaxVisibilityMeshlets || lodCount > 1000000u || primitiveCount > 1000000u || clusterCount > 1000000u || dagNodeCount > 2000000u || dagEdgeCount > 4000000u || boundaryCount > 100000000u || adjacencyCount > 100000000u) return cacheError("cache counts are unreasonable");
    std::uint64_t expectedPayloadSize = 21u * sizeof(std::uint32_t);
    const auto addTableSize = [&](std::uint64_t count, std::uint64_t stride)
    {
        if (count > (std::numeric_limits<std::uint64_t>::max() - expectedPayloadSize) / stride)
            return false;
        expectedPayloadSize += count * stride;
        return true;
    };
    if (!addTableSize(vertexCount, 48u) ||
        !addTableSize(meshletVertexCount, sizeof(std::uint32_t)) ||
        !addTableSize(triangleByteCount, sizeof(std::uint8_t)) ||
        !addTableSize(indexCount, sizeof(std::uint32_t)) ||
        !addTableSize(meshletCount, 68u) || !addTableSize(lodCount, 28u) ||
        !addTableSize(primitiveCount, 36u) ||
         !addTableSize(clusterCount, 60u) || !addTableSize(dagNodeCount, 44u) ||
         !addTableSize(dagEdgeCount, 8u) || !addTableSize(boundaryCount, 4u) ||
         !addTableSize(adjacencyCount, 4u) ||
        expectedPayloadSize > payload.size())
        return cacheError("cache table sizes do not match the payload");
    VirtualGeometryAsset asset;
    try
    {
        asset.vertices.resize(vertexCount);
        asset.meshletVertices.resize(meshletVertexCount);
        asset.meshletTriangles.resize(triangleByteCount);
        asset.indices.resize(indexCount);
        asset.meshlets.resize(meshletCount);
        asset.lods.resize(lodCount);
        asset.primitives.resize(primitiveCount);
        asset.clusters.resize(clusterCount);
        asset.dagNodes.resize(dagNodeCount);
        asset.dagEdges.resize(dagEdgeCount);
    }
    catch (const std::bad_alloc&) { return cacheAllocationError(); }
    for (auto& vertex : asset.vertices) if (!reader.readBytes(&vertex.position, sizeof(vertex.position)) || !reader.readBytes(&vertex.normal, sizeof(vertex.normal)) || !reader.readBytes(&vertex.uv, sizeof(vertex.uv)) || !reader.readBytes(&vertex.tangent, sizeof(vertex.tangent))) return cacheError("vertex data is truncated");
    for (auto& value : asset.meshletVertices) if (!reader.read32(value)) return cacheError("meshlet vertex data is truncated");
    if (!reader.readBytes(asset.meshletTriangles.data(), asset.meshletTriangles.size())) return cacheError("meshlet triangle data is truncated");
    for (auto& value : asset.indices) if (!reader.read32(value)) return cacheError("index data is truncated");
    for (auto& meshlet : asset.meshlets) if (!reader.read32(meshlet.vertexOffset) || !reader.read32(meshlet.vertexCount) || !reader.read32(meshlet.triangleOffset) || !reader.read32(meshlet.triangleCount) || !reader.read32(meshlet.indexOffset) || !reader.read32(meshlet.indexCount) || !reader.read32(meshlet.primitiveIndex) || !reader.read32(meshlet.lodIndex) || !reader.readBytes(&meshlet.sphere, sizeof(meshlet.sphere)) || !reader.readBytes(&meshlet.cone, sizeof(meshlet.cone)) || !reader.readFloat(meshlet.geometricError)) return cacheError("meshlet table is truncated");
    for (auto& lod : asset.lods) if (!reader.read32(lod.primitiveIndex) || !reader.read32(lod.meshletOffset) || !reader.read32(lod.meshletCount) || !reader.read32(lod.indexOffset) || !reader.read32(lod.indexCount) || !reader.readFloat(lod.geometricError) || !reader.readFloat(lod.ratio)) return cacheError("LOD table is truncated");
    for (auto& primitive : asset.primitives) if (!reader.read32(primitive.vertexOffset) || !reader.read32(primitive.vertexCount) || !reader.read32(primitive.materialIndex) || !reader.readBytes(&primitive.boundsMin, sizeof(primitive.boundsMin)) || !reader.readBytes(&primitive.boundsMax, sizeof(primitive.boundsMax))) return cacheError("primitive table is truncated");
    std::uint64_t observedBoundaryCount = 0;
    std::uint64_t observedAdjacencyCount = 0;
    for (auto& cluster : asset.clusters)
    {
        std::uint32_t count = 0;
        std::uint32_t meshletIndexCount = 0;
        std::uint32_t adjacencyEntryCount = 0;
        if (!reader.read32(cluster.meshletOffset) || !reader.read32(cluster.meshletCount) ||
            !reader.read32(cluster.vertexOffset) || !reader.read32(cluster.vertexCount) ||
            !reader.read32(cluster.triangleCount) || !reader.read32(cluster.lodDepth) ||
            !reader.read32(cluster.primitiveIndex) ||
            !reader.readBytes(&cluster.sphere, sizeof(cluster.sphere)) ||
            !reader.readFloat(cluster.geometricError) || !reader.read32(meshletIndexCount))
            return cacheError("cluster table is truncated");
        if (meshletIndexCount != cluster.meshletCount || meshletIndexCount > meshletCount ||
            meshletIndexCount > reader.data.size() / sizeof(std::uint32_t))
            return cacheError("cluster meshlet index count is invalid");
        cluster.meshletIndices.resize(meshletIndexCount);
        for (auto& value : cluster.meshletIndices)
            if (!reader.read32(value)) return cacheError("cluster meshlet index table is truncated");
        if (!reader.read32(count) || observedBoundaryCount + count > boundaryCount)
            return cacheError("cluster boundary table is truncated");
        cluster.boundaryVertices.resize(count);
        for (auto& value : cluster.boundaryVertices) if (!reader.read32(value)) return cacheError("boundary table is truncated");
        observedBoundaryCount += count;
        if (!reader.read32(adjacencyEntryCount) || observedAdjacencyCount + adjacencyEntryCount > adjacencyCount)
            return cacheError("cluster adjacency table is truncated");
        cluster.adjacentClusters.resize(adjacencyEntryCount);
        for (auto& value : cluster.adjacentClusters)
            if (!reader.read32(value)) return cacheError("cluster adjacency table is truncated");
        observedAdjacencyCount += adjacencyEntryCount;
    }
    for (auto& node : asset.dagNodes) if (!reader.read32(node.clusterIndex) || !reader.read32(node.parentIndex) || !reader.read32(node.firstChild) || !reader.read32(node.childCount) || !reader.read32(node.lodDepth) || !reader.read32(node.flags) || !reader.readBytes(&node.sphere, sizeof(node.sphere)) || !reader.readFloat(node.geometricError)) return cacheError("DAG node table is truncated");
    for (auto& edge : asset.dagEdges) if (!reader.read32(edge.parent) || !reader.read32(edge.child)) return cacheError("DAG edge table is truncated");
    if (observedBoundaryCount != boundaryCount || observedAdjacencyCount != adjacencyCount)
        return cacheError("cluster auxiliary table count mismatch");
    if (reader.offset != payload.size()) return cacheError("cache payload has trailing bytes");
    if (const auto invalid = validateAsset(asset, serializedBuild))
        return cacheError(std::string(*invalid));
    if (!validateVirtualGeometryDag(asset))
        return cacheError("cache M6 DAG is invalid");
    if (metisVersion != ((METIS_VER_MAJOR << 16) | (METIS_VER_MINOR << 8) | METIS_VER_SUBMINOR) ||
        metisUfactor == 0u || storedDagChecksum != dagChecksum(asset))
        return cacheError("cache METIS metadata or DAG checksum mismatch");
    if (metadata) { metadata->build.maxVertices = maxVertices; metadata->build.maxTriangles = maxTriangles; metadata->build.simplifyError = simplifyError; metadata->build.lodRatios = lodRatios; metadata->build.metisSeed = metisSeed; metadata->build.metisUfactor = metisUfactor; metadata->build.maxClusterVertices = maxClusterVertices; metadata->build.maxClusterTriangles = maxClusterTriangles; metadata->build.maxDagDepth = maxDagDepth; metadata->build.lodRefineThresholdPixels = refineThreshold; metadata->build.lodCoarsenThresholdPixels = coarsenThreshold; metadata->compressionLevel = level; metadata->pageSize = cacheMetadata->options.pageSize; }
    return Halcyon::Result<VirtualGeometryAsset>::success(std::move(asset));
}

Halcyon::Result<void> writeVirtualGeometryCache(const std::filesystem::path& path,
    const VirtualGeometryAsset& asset, const Sha256Digest& sourceHash,
    const VirtualGeometryCacheOptions& options)
{
    if (options.compressionLevel > static_cast<std::uint32_t>(ZSTD_maxCLevel()) ||
        options.pageSize < 4096u || options.pageSize > 1024u * 1024u ||
        (options.pageSize & (options.pageSize - 1u)) != 0u)
        return Halcyon::Err({Halcyon::ErrorCode::InvalidArgument,
            "cache compression level or page size is invalid", path.string()});
    if (const auto invalid = validateBuildOptions(options.build))
        return Halcyon::Err({Halcyon::ErrorCode::InvalidArgument,
            std::string(*invalid), path.string()});
    if (const auto invalid = validateAsset(asset, options.build))
        return Halcyon::Err({Halcyon::ErrorCode::InvalidArgument,
            std::string(*invalid), path.string()});

    const auto pageLayout = buildVirtualGeometryPageLayout(asset, options.pageSize);
    if (!pageLayout)
        return Halcyon::Result<void>::failure(pageLayout.error());
    if (pageLayout->pageCount == 0u || pageLayout->pageCount > kMaxCachePageCount ||
        pageLayout->rawSize > kMaxCachePayloadBytes ||
        pageLayout->rootPageIndices.empty())
        return Halcyon::Err({Halcyon::ErrorCode::InvalidArgument,
            "cache geometry page layout exceeds the supported range", path.string()});
    const auto resident = serializeResidentMetadata(asset, options, pageLayout.value());
    if (!resident)
        return Halcyon::Result<void>::failure(resident.error());

    std::uint64_t rootTableSize = 0u;
    std::uint64_t directorySize = 0u;
    if (!tableByteSize(pageLayout->rootPageIndices.size(), sizeof(std::uint32_t),
            rootTableSize) ||
        !tableByteSize(pageLayout->pageCount, kPageDirectoryEntrySize, directorySize))
        return Halcyon::Err({Halcyon::ErrorCode::InvalidArgument,
            "cache table size overflow", path.string()});
    const std::uint64_t metadataOffset = kHeaderSize;
    std::uint64_t rootTableOffset = 0u, directoryOffset = 0u, payloadOffset = 0u;
    if (!checkedAdd(metadataOffset, resident->size(), rootTableOffset) ||
        !checkedAdd(rootTableOffset, rootTableSize, directoryOffset) ||
        !checkedAdd(directoryOffset, directorySize, payloadOffset))
        return Halcyon::Err({Halcyon::ErrorCode::InvalidArgument,
            "cache file layout overflow", path.string()});

    std::vector<std::byte> header;
    header.reserve(kHeaderSize);
    header.insert(header.end(), reinterpret_cast<const std::byte*>(kMagic.data()),
        reinterpret_cast<const std::byte*>(kMagic.data() + kMagic.size()));
    append32(header, kVirtualGeometryCacheVersion);
    append32(header, kEndian);
    appendBytes(header, sourceHash.data(), sourceHash.size());
    append32(header, options.pageSize);
    append32(header, pageLayout->pageCount);
    append32(header, options.compressionLevel);
    append32(header, MESHOPTIMIZER_VERSION);
    append64(header, metadataOffset);
    append64(header, resident->size());
    append64(header, rootTableOffset);
    append64(header, rootTableSize);
    append64(header, directoryOffset);
    append64(header, directorySize);
    append64(header, payloadOffset);
    append64(header, pageLayout->rawSize);
    if (header.size() != kHeaderSize)
        return Halcyon::Err({Halcyon::ErrorCode::InvalidState,
            "cache header ABI size mismatch", path.string()});

    const std::filesystem::path temporary = path.string() + ".tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream)
        return Halcyon::Err({Halcyon::ErrorCode::Io,
            "unable to create cache", path.string()});
    const auto failWrite = [&](std::string message)
    {
        stream.close();
        std::error_code removeError;
        std::filesystem::remove(temporary, removeError);
        return Halcyon::Err({Halcyon::ErrorCode::Io, std::move(message), path.string()});
    };
    stream.write(reinterpret_cast<const char*>(header.data()),
        static_cast<std::streamsize>(header.size()));
    stream.write(reinterpret_cast<const char*>(resident->data()),
        static_cast<std::streamsize>(resident->size()));
    for (const auto page : pageLayout->rootPageIndices)
    {
        std::array<std::byte, sizeof(std::uint32_t)> encoded{};
        std::vector<std::byte> value;
        value.reserve(sizeof(std::uint32_t));
        append32(value, page);
        std::copy(value.begin(), value.end(), encoded.begin());
        stream.write(reinterpret_cast<const char*>(encoded.data()),
            static_cast<std::streamsize>(encoded.size()));
    }
    std::vector<std::byte> zeroDirectory;
    try
    {
        zeroDirectory.resize(static_cast<std::size_t>(directorySize), std::byte{0});
    }
    catch (const std::bad_alloc&)
    {
        return failWrite("unable to allocate cache page directory");
    }
    stream.write(reinterpret_cast<const char*>(zeroDirectory.data()),
        static_cast<std::streamsize>(zeroDirectory.size()));
    if (!stream)
        return failWrite("unable to write cache metadata");

    ZSTD_CCtx* context = ZSTD_createCCtx();
    if (context == nullptr)
        return failWrite("unable to create zstd context");
    const auto releaseContext = [&]() { ZSTD_freeCCtx(context); context = nullptr; };
    const std::size_t levelResult = ZSTD_CCtx_setParameter(context,
        ZSTD_c_compressionLevel, static_cast<int>(options.compressionLevel));
    const std::size_t checksumResult = ZSTD_CCtx_setParameter(
        context, ZSTD_c_checksumFlag, 1);
    if (ZSTD_isError(levelResult) || ZSTD_isError(checksumResult))
    {
        releaseContext();
        return failWrite("zstd page compression parameters are unsupported");
    }

    std::vector<VirtualGeometryCachePageEntry> entries(pageLayout->pageCount);
    std::uint64_t fileOffset = payloadOffset;
    for (std::uint32_t pageIndex = 0u; pageIndex < pageLayout->pageCount; ++pageIndex)
    {
        const auto rawPage = serializeVirtualGeometryPage(
            asset, pageLayout.value(), pageIndex);
        if (!rawPage)
        {
            releaseContext();
            return failWrite(rawPage.error().describe());
        }
        std::vector<std::byte> compressed;
        try
        {
            compressed.resize(ZSTD_compressBound(rawPage->size()));
        }
        catch (const std::bad_alloc&)
        {
            releaseContext();
            return failWrite("unable to allocate compressed geometry page");
        }
        const std::size_t compressedSize = ZSTD_compress2(context,
            compressed.data(), compressed.size(), rawPage->data(), rawPage->size());
        if (ZSTD_isError(compressedSize) ||
            compressedSize > std::numeric_limits<std::uint32_t>::max())
        {
            const std::string reason = ZSTD_isError(compressedSize)
                ? ZSTD_getErrorName(compressedSize)
                : "compressed geometry page exceeds the integer range";
            releaseContext();
            return failWrite(reason);
        }
        compressed.resize(compressedSize);
        auto& entry = entries[pageIndex];
        entry.fileOffset = fileOffset;
        entry.rawOffset = static_cast<std::uint64_t>(pageIndex) * options.pageSize;
        entry.compressedSize = static_cast<std::uint32_t>(compressedSize);
        entry.rawSize = options.pageSize;
        entry.crc32 = crc32(rawPage.value());
        entry.flags = std::binary_search(pageLayout->rootPageIndices.begin(),
            pageLayout->rootPageIndices.end(), pageIndex)
            ? VirtualGeometryCachePagePinnedRoot : VirtualGeometryCachePageNone;
        stream.write(reinterpret_cast<const char*>(compressed.data()),
            static_cast<std::streamsize>(compressed.size()));
        if (!stream || !checkedAdd(fileOffset, compressedSize, fileOffset))
        {
            releaseContext();
            return failWrite("unable to write compressed geometry page");
        }
    }
    releaseContext();

    stream.seekp(static_cast<std::streamoff>(directoryOffset));
    for (const auto& entry : entries)
    {
        std::vector<std::byte> encoded;
        encoded.reserve(kPageDirectoryEntrySize);
        append64(encoded, entry.fileOffset);
        append64(encoded, entry.rawOffset);
        append32(encoded, entry.compressedSize);
        append32(encoded, entry.rawSize);
        append32(encoded, entry.crc32);
        append32(encoded, entry.flags);
        stream.write(reinterpret_cast<const char*>(encoded.data()),
            static_cast<std::streamsize>(encoded.size()));
    }
    stream.close();
    if (!stream)
        return failWrite("unable to finalize cache page directory");
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error)
    {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
    }
    if (error)
    {
        std::filesystem::remove(temporary, error);
        return Halcyon::Err({Halcyon::ErrorCode::Io,
            "unable to publish cache", path.string()});
    }
    return Halcyon::Ok();
}

Halcyon::Result<VirtualGeometryCacheMetadata> readVirtualGeometryCacheMetadata(
    const std::filesystem::path& path, const Sha256Digest* expectedSourceHash,
    const VirtualGeometryCacheOptions* expectedOptions)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        return typedCacheError<VirtualGeometryCacheMetadata>("cache does not exist");
    const auto signedSize = stream.tellg();
    if (signedSize < 0 || static_cast<std::uint64_t>(signedSize) < kHeaderSize)
        return typedCacheError<VirtualGeometryCacheMetadata>("cache is truncated");
    const std::uint64_t fileSize = static_cast<std::uint64_t>(signedSize);
    std::array<std::byte, kHeaderSize> fileHeader{};
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(fileHeader.data()),
        static_cast<std::streamsize>(fileHeader.size()));
    if (!stream || std::memcmp(fileHeader.data(), kMagic.data(), kMagic.size()) != 0)
        return typedCacheError<VirtualGeometryCacheMetadata>(
            !stream ? "unable to read cache" : "cache magic mismatch");

    Reader header{fileHeader};
    header.offset = kMagic.size();
    std::uint32_t version = 0u, endian = 0u, pageSize = 0u, pageCount = 0u;
    std::uint32_t compressionLevel = 0u, meshoptVersion = 0u;
    std::uint64_t metadataOffset = 0u, metadataSize = 0u;
    std::uint64_t rootTableOffset = 0u, rootTableSize = 0u;
    std::uint64_t directoryOffset = 0u, directorySize = 0u;
    std::uint64_t payloadOffset = 0u, rawSize = 0u;
    Sha256Digest sourceHash{};
    if (!header.read32(version) || !header.read32(endian) ||
        !header.readBytes(sourceHash.data(), sourceHash.size()) ||
        !header.read32(pageSize) || !header.read32(pageCount) ||
        !header.read32(compressionLevel) || !header.read32(meshoptVersion) ||
        !header.read64(metadataOffset) || !header.read64(metadataSize) ||
        !header.read64(rootTableOffset) || !header.read64(rootTableSize) ||
        !header.read64(directoryOffset) || !header.read64(directorySize) ||
        !header.read64(payloadOffset) || !header.read64(rawSize))
        return typedCacheError<VirtualGeometryCacheMetadata>("cache header is truncated");
    if (version != kVirtualGeometryCacheVersion)
        return typedCacheError<VirtualGeometryCacheMetadata>("cache version " +
            std::to_string(version) + " is incompatible with v5; re-run HalcyonCooker");
    std::uint64_t expectedRootSize = 0u, expectedDirectorySize = 0u;
    if (endian != kEndian || meshoptVersion != MESHOPTIMIZER_VERSION ||
        compressionLevel > static_cast<std::uint32_t>(ZSTD_maxCLevel()) ||
        pageSize < 4096u || pageSize > 1024u * 1024u ||
        (pageSize & (pageSize - 1u)) != 0u || pageCount == 0u ||
        pageCount > kMaxCachePageCount || metadataOffset != kHeaderSize ||
        metadataSize == 0u || metadataSize > kMaxResidentMetadataBytes ||
        !checkedAdd(metadataOffset, metadataSize, expectedRootSize) ||
        rootTableOffset != expectedRootSize || rootTableSize == 0u ||
        rootTableSize % sizeof(std::uint32_t) != 0u ||
        !checkedAdd(rootTableOffset, rootTableSize, expectedRootSize) ||
        directoryOffset != expectedRootSize ||
        !tableByteSize(pageCount, kPageDirectoryEntrySize, expectedDirectorySize) ||
        directorySize != expectedDirectorySize ||
        !checkedAdd(directoryOffset, directorySize, expectedDirectorySize) ||
        payloadOffset != expectedDirectorySize || payloadOffset > fileSize ||
        rawSize != static_cast<std::uint64_t>(pageCount) * pageSize ||
        rawSize > kMaxCachePayloadBytes)
        return typedCacheError<VirtualGeometryCacheMetadata>("cache header is invalid");
    if (expectedSourceHash != nullptr && *expectedSourceHash != sourceHash)
        return typedCacheError<VirtualGeometryCacheMetadata>("cache source hash mismatch");

    VirtualGeometryCacheMetadata result;
    result.sourceHash = sourceHash;
    result.options.compressionLevel = compressionLevel;
    result.options.pageSize = pageSize;
    result.rawPayloadSize = rawSize;
    std::vector<std::byte> residentBytes;
    try
    {
        residentBytes.resize(static_cast<std::size_t>(metadataSize));
    }
    catch (const std::bad_alloc&)
    {
        return Halcyon::Result<VirtualGeometryCacheMetadata>::failure(
            {Halcyon::ErrorCode::OutOfMemory,
                "unable to allocate resident metadata", "VirtualGeometryCache"});
    }
    stream.seekg(static_cast<std::streamoff>(metadataOffset));
    stream.read(reinterpret_cast<char*>(residentBytes.data()),
        static_cast<std::streamsize>(residentBytes.size()));
    if (!stream)
        return typedCacheError<VirtualGeometryCacheMetadata>(
            "resident metadata is truncated");
    const auto decoded = deserializeResidentMetadata(residentBytes, result);
    if (!decoded)
        return Halcyon::Result<VirtualGeometryCacheMetadata>::failure(decoded.error());
    if (result.pageLayout.pageSize != pageSize ||
        result.pageLayout.pageCount != pageCount || result.pageLayout.rawSize != rawSize)
        return typedCacheError<VirtualGeometryCacheMetadata>(
            "resident metadata disagrees with the cache header");
    if (expectedOptions != nullptr &&
        (expectedOptions->build.lodRatios != result.options.build.lodRatios ||
            expectedOptions->build.maxVertices != result.options.build.maxVertices ||
            expectedOptions->build.maxTriangles != result.options.build.maxTriangles ||
            expectedOptions->build.simplifyError != result.options.build.simplifyError ||
            expectedOptions->build.maxClusterVertices !=
                result.options.build.maxClusterVertices ||
            expectedOptions->build.maxClusterTriangles !=
                result.options.build.maxClusterTriangles ||
            expectedOptions->build.metisSeed != result.options.build.metisSeed ||
            expectedOptions->build.metisUfactor != result.options.build.metisUfactor ||
            expectedOptions->build.maxDagDepth != result.options.build.maxDagDepth ||
            expectedOptions->build.lodRefineThresholdPixels !=
                result.options.build.lodRefineThresholdPixels ||
            expectedOptions->build.lodCoarsenThresholdPixels !=
                result.options.build.lodCoarsenThresholdPixels ||
            expectedOptions->compressionLevel != compressionLevel ||
            expectedOptions->pageSize != pageSize))
        return typedCacheError<VirtualGeometryCacheMetadata>(
            "cache cooker parameters mismatch");

    result.rootPageCount = static_cast<std::uint32_t>(
        rootTableSize / sizeof(std::uint32_t));
    if (result.rootPageCount == 0u || result.rootPageCount > pageCount)
        return typedCacheError<VirtualGeometryCacheMetadata>("root page table is invalid");
    std::vector<std::byte> rootBytes(static_cast<std::size_t>(rootTableSize));
    stream.seekg(static_cast<std::streamoff>(rootTableOffset));
    stream.read(reinterpret_cast<char*>(rootBytes.data()),
        static_cast<std::streamsize>(rootBytes.size()));
    if (!stream)
        return typedCacheError<VirtualGeometryCacheMetadata>("root page table is truncated");
    Reader rootReader{rootBytes};
    result.rootPageIndices.resize(result.rootPageCount);
    for (std::uint32_t i = 0u; i < result.rootPageCount; ++i)
    {
        if (!rootReader.read32(result.rootPageIndices[i]) ||
            result.rootPageIndices[i] >= pageCount ||
            (i != 0u && result.rootPageIndices[i - 1u] >= result.rootPageIndices[i]))
            return typedCacheError<VirtualGeometryCacheMetadata>(
                "root page table is not sorted and valid");
    }
    result.pageLayout.rootPageIndices = result.rootPageIndices;

    std::vector<std::byte> directoryBytes(static_cast<std::size_t>(directorySize));
    stream.seekg(static_cast<std::streamoff>(directoryOffset));
    stream.read(reinterpret_cast<char*>(directoryBytes.data()),
        static_cast<std::streamsize>(directoryBytes.size()));
    if (!stream)
        return typedCacheError<VirtualGeometryCacheMetadata>(
            "cache page directory is truncated");
    Reader directoryReader{directoryBytes};
    result.pages.resize(pageCount);
    std::uint64_t expectedFileOffset = payloadOffset;
    for (std::uint32_t pageIndex = 0u; pageIndex < pageCount; ++pageIndex)
    {
        auto& page = result.pages[pageIndex];
        if (!directoryReader.read64(page.fileOffset) ||
            !directoryReader.read64(page.rawOffset) ||
            !directoryReader.read32(page.compressedSize) ||
            !directoryReader.read32(page.rawSize) ||
            !directoryReader.read32(page.crc32) ||
            !directoryReader.read32(page.flags))
            return typedCacheError<VirtualGeometryCacheMetadata>(
                "cache page directory is truncated");
        const bool pinned = std::binary_search(result.rootPageIndices.begin(),
            result.rootPageIndices.end(), pageIndex);
        if (page.fileOffset != expectedFileOffset ||
            page.rawOffset != static_cast<std::uint64_t>(pageIndex) * pageSize ||
            page.compressedSize < 5u || page.rawSize != pageSize ||
            page.flags != (pinned ? VirtualGeometryCachePagePinnedRoot :
                VirtualGeometryCachePageNone) || page.fileOffset > fileSize ||
            page.compressedSize > fileSize - page.fileOffset ||
            !checkedAdd(expectedFileOffset, page.compressedSize, expectedFileOffset))
            return typedCacheError<VirtualGeometryCacheMetadata>(
                "cache page directory entry is invalid");
    }
    if (directoryReader.offset != directoryBytes.size() || expectedFileOffset != fileSize)
        return typedCacheError<VirtualGeometryCacheMetadata>(
            "cache page directory does not cover the payload");
    return Halcyon::Result<VirtualGeometryCacheMetadata>::success(std::move(result));
}

Halcyon::Result<VirtualGeometryAsset> readVirtualGeometryCache(
    const std::filesystem::path& path, const Sha256Digest* expectedSourceHash,
    VirtualGeometryCacheOptions* metadata,
    const VirtualGeometryCacheOptions* expectedOptions)
{
    auto cacheMetadata = readVirtualGeometryCacheMetadata(
        path, expectedSourceHash, expectedOptions);
    if (!cacheMetadata)
        return Halcyon::Result<VirtualGeometryAsset>::failure(cacheMetadata.error());
    VirtualGeometryAsset asset = std::move(cacheMetadata->residentAsset);
    const auto& layout = cacheMetadata->pageLayout;
    try
    {
        asset.vertices.resize(layout.vertices.size / sizeof(StaticSceneVertex));
        asset.meshletVertices.resize(
            layout.meshletVertices.size / sizeof(std::uint32_t));
        asset.meshletTriangles.resize(layout.meshletTriangles.size);
        asset.indices.resize(layout.indices.size / sizeof(std::uint32_t));
    }
    catch (const std::bad_alloc&)
    {
        return cacheAllocationError();
    }
    for (std::uint32_t pageIndex = 0u; pageIndex < cacheMetadata->pages.size(); ++pageIndex)
    {
        auto page = readVirtualGeometryCachePage(path, cacheMetadata.value(), pageIndex);
        if (!page)
            return Halcyon::Result<VirtualGeometryAsset>::failure(page.error());
        const auto unpacked = unpackVirtualGeometryPage(
            asset, layout, pageIndex, page.value());
        if (!unpacked)
            return Halcyon::Result<VirtualGeometryAsset>::failure(unpacked.error());
    }
    if (const auto invalid = validateAsset(asset, cacheMetadata->options.build))
        return cacheError(std::string(*invalid));
    if (!validateVirtualGeometryDag(asset))
        return cacheError("cache M6 DAG is invalid");
    if (metadata != nullptr)
        *metadata = cacheMetadata->options;
    return Halcyon::Result<VirtualGeometryAsset>::success(std::move(asset));
}

} // namespace Halcyon::Renderer::Scene
