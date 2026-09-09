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
constexpr std::size_t kMaxVisibilityMeshlets = kVirtualVisibilityMeshletMask;
constexpr std::uint64_t kHeaderSize = 104u;
constexpr std::uint64_t kMaxCachePayloadBytes = 2ull * 1024ull * 1024ull * 1024ull;

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

} // namespace

Halcyon::Result<void> writeVirtualGeometryCache(const std::filesystem::path& path,
    const VirtualGeometryAsset& asset, const Sha256Digest& sourceHash,
    const VirtualGeometryCacheOptions& options)
{
    if (options.compressionLevel > static_cast<std::uint32_t>(ZSTD_maxCLevel()))
        return Halcyon::Err({Halcyon::ErrorCode::InvalidArgument,
            "cache options are invalid", path.string()});
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
    const std::size_t compressedBound = ZSTD_compressBound(payload.size());
    std::vector<std::byte> compressed(compressedBound);
    ZSTD_CCtx* context = ZSTD_createCCtx();
    if (!context) return Halcyon::Err({Halcyon::ErrorCode::Io, "unable to create zstd context", path.string()});
    const std::size_t parameterResult = ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel, static_cast<int>(options.compressionLevel));
    const std::size_t checksumResult = ZSTD_CCtx_setParameter(context, ZSTD_c_checksumFlag, 1);
    const std::size_t compressedSize = ZSTD_isError(parameterResult) || ZSTD_isError(checksumResult)
        ? ZSTD_error_parameter_unsupported : ZSTD_compress2(context, compressed.data(), compressed.size(), payload.data(), payload.size());
    ZSTD_freeCCtx(context);
    if (ZSTD_isError(compressedSize)) return Halcyon::Err({Halcyon::ErrorCode::Io, ZSTD_getErrorName(compressedSize), "zstd compression"});
    compressed.resize(compressedSize);
    std::vector<std::byte> header;
    header.insert(header.end(), reinterpret_cast<const std::byte*>(kMagic.data()), reinterpret_cast<const std::byte*>(kMagic.data() + kMagic.size()));
    append32(header, kVirtualGeometryCacheVersion); append32(header, kEndian); appendBytes(header, sourceHash.data(), sourceHash.size());
    append32(header, options.build.maxVertices); append32(header, options.build.maxTriangles); append32(header, options.compressionLevel);
    append32(header, MESHOPTIMIZER_VERSION); appendFloat(header, options.build.simplifyError);
    for (float ratio : options.build.lodRatios) appendFloat(header, ratio);
    append64(header, kHeaderSize);
    append64(header, payload.size()); append64(header, compressed.size());
    if (header.size() != kHeaderSize)
        return Halcyon::Err({Halcyon::ErrorCode::InvalidState,
            "cache header ABI size mismatch", path.string()});
    const auto temporary = path.string() + ".tmp"; std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) return Halcyon::Err({Halcyon::ErrorCode::Io, "unable to create cache", path.string()});
    stream.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    stream.write(reinterpret_cast<const char*>(compressed.data()), static_cast<std::streamsize>(compressed.size())); stream.close();
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

Halcyon::Result<VirtualGeometryAsset> readVirtualGeometryCache(const std::filesystem::path& path,
    const Sha256Digest* expectedSourceHash, VirtualGeometryCacheOptions* metadata,
    const VirtualGeometryCacheOptions* expectedOptions)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) return cacheError("cache does not exist");
    const auto size = stream.tellg(); if (size < 0 || static_cast<std::uint64_t>(size) < kHeaderSize ||
        static_cast<std::uintmax_t>(size) > std::numeric_limits<std::size_t>::max() ||
        static_cast<std::uintmax_t>(size) - kHeaderSize > 2ull * 1024ull * 1024ull * 1024ull)
        return cacheError("cache is truncated");
    std::array<std::byte, kHeaderSize> fileHeader{};
    stream.seekg(0); stream.read(reinterpret_cast<char*>(fileHeader.data()),
        static_cast<std::streamsize>(fileHeader.size()));
    if (!stream) return cacheError("unable to read cache");
    if (std::memcmp(fileHeader.data(), kMagic.data(), kMagic.size()) != 0) return cacheError("cache magic mismatch");
    Reader header{fileHeader}; header.offset = 8; std::uint32_t version = 0, endian = 0, maxVertices = 0, maxTriangles = 0, level = 0, meshoptVersion = 0; float simplifyError = 0.0f; std::array<float, 3> lodRatios{}; std::uint64_t payloadOffset = 0, rawSize = 0, compressedSize = 0;
    Sha256Digest source{};
    if (!header.read32(version) || !header.read32(endian) ||
        !header.readBytes(source.data(), source.size()) || !header.read32(maxVertices) ||
        !header.read32(maxTriangles) || !header.read32(level) ||
        !header.read32(meshoptVersion) || !header.readFloat(simplifyError) ||
        !header.readFloat(lodRatios[0]) || !header.readFloat(lodRatios[1]) ||
        !header.readFloat(lodRatios[2]) || !header.read64(payloadOffset) ||
        !header.read64(rawSize) || !header.read64(compressedSize))
        return cacheError("cache header is truncated");
    const bool invalidRatio = std::any_of(lodRatios.begin(), lodRatios.end(),
        [](float ratio) { return !std::isfinite(ratio) || ratio < 0.0f || ratio > 1.0f; });
    std::uint32_t metisSeed = 0u;
    std::uint32_t metisUfactor = 0u;
    std::uint32_t maxClusterVertices = 0u;
    std::uint32_t maxClusterTriangles = 0u;
    std::uint32_t maxDagDepth = 0u;
    float refineThreshold = 0.0f;
    float coarsenThreshold = 0.0f;
    VirtualGeometryBuildOptions serializedBuild{};
    serializedBuild.maxVertices = maxVertices;
    serializedBuild.maxTriangles = maxTriangles;
    serializedBuild.simplifyError = simplifyError;
    serializedBuild.lodRatios = lodRatios;
    serializedBuild.metisSeed = 0u;
    serializedBuild.metisUfactor = 1u;
    if (version != kVirtualGeometryCacheVersion)
        return cacheError("cache version " + std::to_string(version) +
            " is incompatible with v4; re-run HalcyonCooker");
    if (endian != kEndian || meshoptVersion != MESHOPTIMIZER_VERSION || invalidRatio ||
        validateBuildOptions(serializedBuild).has_value() ||
        level > static_cast<std::uint32_t>(ZSTD_maxCLevel()) ||
        payloadOffset != kHeaderSize || payloadOffset != header.offset ||
        rawSize > kMaxCachePayloadBytes || rawSize > std::numeric_limits<std::size_t>::max() ||
        compressedSize > kMaxCachePayloadBytes ||
        compressedSize != static_cast<std::uint64_t>(size) - header.offset)
        return cacheError("cache header is invalid");
    if (expectedSourceHash && *expectedSourceHash != source) return cacheError("cache source hash mismatch");
    if (expectedOptions != nullptr &&
        (expectedOptions->build.maxVertices != maxVertices ||
            expectedOptions->build.maxTriangles != maxTriangles ||
            expectedOptions->build.simplifyError != simplifyError ||
            expectedOptions->build.lodRatios != lodRatios ||
            expectedOptions->compressionLevel != level))
        return cacheError("cache cooker parameters mismatch");
    // Keep cache loading on zstd's public API. The frame-header inspection
    // structs are static-linking-only in newer zstd releases, but the frame
    // descriptor's checksum bit is stable: bit 2 of the first frame header
    // byte. Decompression below still performs the actual checksum check.
    // The frame header is inspected below, so reject an empty/short payload
    // before touching its first bytes.  A header-only file is a valid shape
    // for a truncation test but must never turn into an out-of-bounds read.
    if (compressedSize < 5u)
        return cacheError("cache payload is truncated");
    std::vector<std::byte> compressed;
    try { compressed.resize(static_cast<std::size_t>(compressedSize)); }
    catch (const std::bad_alloc&) { return cacheAllocationError(); }
    stream.read(reinterpret_cast<char*>(compressed.data()),
        static_cast<std::streamsize>(compressed.size()));
    if (!stream) return cacheError("unable to read cache payload");
    const auto* compressedBytes = reinterpret_cast<const std::uint8_t*>(
        compressed.data());
    constexpr std::uint32_t kZstdFrameMagic = 0xFD2FB528u;
    const std::uint32_t frameMagic = static_cast<std::uint32_t>(compressedBytes[0]) |
        (static_cast<std::uint32_t>(compressedBytes[1]) << 8u) |
        (static_cast<std::uint32_t>(compressedBytes[2]) << 16u) |
        (static_cast<std::uint32_t>(compressedBytes[3]) << 24u);
    if (frameMagic != kZstdFrameMagic ||
        (compressedBytes[4] & 0x04u) == 0u)
        return cacheError("cache payload is missing a zstd checksum");
    const auto frameContentSize = ZSTD_getFrameContentSize(compressedBytes,
        static_cast<std::size_t>(compressedSize));
    if (frameContentSize == ZSTD_CONTENTSIZE_ERROR ||
        frameContentSize == ZSTD_CONTENTSIZE_UNKNOWN || frameContentSize != rawSize)
        return cacheError("cache payload frame size is invalid");
    std::vector<std::byte> payload;
    try { payload.resize(static_cast<std::size_t>(rawSize)); }
    catch (const std::bad_alloc&) { return cacheAllocationError(); }
    const auto result = ZSTD_decompress(payload.data(), payload.size(),
        compressed.data(), compressed.size());
    if (ZSTD_isError(result) || result != rawSize) return cacheError("cache payload checksum or size mismatch");
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
    if (metadata) { metadata->build.maxVertices = maxVertices; metadata->build.maxTriangles = maxTriangles; metadata->build.simplifyError = simplifyError; metadata->build.lodRatios = lodRatios; metadata->build.metisSeed = metisSeed; metadata->build.metisUfactor = metisUfactor; metadata->build.maxClusterVertices = maxClusterVertices; metadata->build.maxClusterTriangles = maxClusterTriangles; metadata->build.maxDagDepth = maxDagDepth; metadata->build.lodRefineThresholdPixels = refineThreshold; metadata->build.lodCoarsenThresholdPixels = coarsenThreshold; metadata->compressionLevel = level; }
    return Halcyon::Result<VirtualGeometryAsset>::success(std::move(asset));
}

} // namespace Halcyon::Renderer::Scene
