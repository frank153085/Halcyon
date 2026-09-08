#include "VirtualGeometryCache.h"

#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>
#include <zstd.h>
#include <meshoptimizer.h>

namespace Halcyon::Renderer::Scene
{
namespace
{

constexpr std::array<char, 8> kMagic{'H', 'A', 'L', 'C', 'Y', 'O', 'N', 'V'};
constexpr std::uint32_t kEndian = 0x01020304u;

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
        if (data.size() - offset < 4u) return false; value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(data[offset++]) << (i * 8u); return true;
    }
    bool read64(std::uint64_t& value)
    {
        if (data.size() - offset < 8u) return false; value = 0;
        for (unsigned i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(data[offset++]) << (i * 8u); return true;
    }
    bool readFloat(float& value) { std::uint32_t bits = 0; if (!read32(bits)) return false; std::memcpy(&value, &bits, sizeof(value)); return true; }
    bool readBytes(void* destination, std::size_t size)
    {
        if (size > data.size() - offset) return false; std::memcpy(destination, data.data() + offset, size); offset += size; return true;
    }
};

Halcyon::Result<VirtualGeometryAsset> cacheError(std::string message)
{
    return Halcyon::Result<VirtualGeometryAsset>::failure(
        {Halcyon::ErrorCode::InvalidArgument, std::move(message), "VirtualGeometryCache"});
}

} // namespace

Halcyon::Result<void> writeVirtualGeometryCache(const std::filesystem::path& path,
    const VirtualGeometryAsset& asset, const Sha256Digest& sourceHash,
    const VirtualGeometryCacheOptions& options)
{
    std::vector<std::byte> payload;
    append32(payload, static_cast<std::uint32_t>(asset.vertices.size()));
    append32(payload, static_cast<std::uint32_t>(asset.meshletVertices.size()));
    append32(payload, static_cast<std::uint32_t>(asset.meshletTriangles.size()));
    append32(payload, static_cast<std::uint32_t>(asset.indices.size()));
    append32(payload, static_cast<std::uint32_t>(asset.meshlets.size()));
    append32(payload, static_cast<std::uint32_t>(asset.lods.size()));
    append32(payload, static_cast<std::uint32_t>(asset.primitives.size()));
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
    append64(header, payload.size()); append64(header, compressed.size());
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

Halcyon::Result<VirtualGeometryAsset> readVirtualGeometryCache(const std::filesystem::path& path,
    const Sha256Digest* expectedSourceHash, VirtualGeometryCacheOptions* metadata)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) return cacheError("cache does not exist");
    const auto size = stream.tellg(); if (size < 0 || static_cast<std::uint64_t>(size) < 84u) return cacheError("cache is truncated");
    std::vector<std::byte> file(static_cast<std::size_t>(size)); stream.seekg(0); stream.read(reinterpret_cast<char*>(file.data()), size);
    if (!stream) return cacheError("unable to read cache");
    if (std::memcmp(file.data(), kMagic.data(), kMagic.size()) != 0) return cacheError("cache magic mismatch");
    Reader header{file}; header.offset = 8; std::uint32_t version = 0, endian = 0, maxVertices = 0, maxTriangles = 0, level = 0, meshoptVersion = 0; float simplifyError = 0.0f; std::uint64_t rawSize = 0, compressedSize = 0;
    Sha256Digest source{}; if (!header.read32(version) || !header.read32(endian) || !header.readBytes(source.data(), source.size()) || !header.read32(maxVertices) || !header.read32(maxTriangles) || !header.read32(level) || !header.read32(meshoptVersion) || !header.readFloat(simplifyError) || !header.read64(rawSize) || !header.read64(compressedSize)) return cacheError("cache header is truncated");
    if (version != kVirtualGeometryCacheVersion || endian != kEndian || meshoptVersion != MESHOPTIMIZER_VERSION || maxVertices == 0 || maxVertices > 256u || maxTriangles == 0 || maxTriangles > 512u || !std::isfinite(simplifyError) || simplifyError < 0.0f || rawSize > std::numeric_limits<std::size_t>::max() || compressedSize > file.size() - header.offset || compressedSize != file.size() - header.offset) return cacheError("cache header is invalid");
    if (expectedSourceHash && *expectedSourceHash != source) return cacheError("cache source hash mismatch");
    std::vector<std::byte> payload(static_cast<std::size_t>(rawSize)); const auto result = ZSTD_decompress(payload.data(), payload.size(), file.data() + header.offset, static_cast<std::size_t>(compressedSize));
    if (ZSTD_isError(result) || result != rawSize) return cacheError("cache payload checksum or size mismatch");
    Reader reader{payload}; std::uint32_t vertexCount = 0, meshletVertexCount = 0, triangleByteCount = 0, indexCount = 0, meshletCount = 0, lodCount = 0, primitiveCount = 0;
    if (!reader.read32(vertexCount) || !reader.read32(meshletVertexCount) || !reader.read32(triangleByteCount) || !reader.read32(indexCount) || !reader.read32(meshletCount) || !reader.read32(lodCount) || !reader.read32(primitiveCount)) return cacheError("cache payload header is truncated");
    if (vertexCount > 100000000u || meshletVertexCount > 100000000u || triangleByteCount > 300000000u || indexCount > 300000000u || meshletCount > 100000000u || lodCount > 1000000u || primitiveCount > 1000000u) return cacheError("cache counts are unreasonable");
    VirtualGeometryAsset asset; asset.vertices.resize(vertexCount); asset.meshletVertices.resize(meshletVertexCount); asset.meshletTriangles.resize(triangleByteCount); asset.indices.resize(indexCount); asset.meshlets.resize(meshletCount); asset.lods.resize(lodCount); asset.primitives.resize(primitiveCount);
    for (auto& vertex : asset.vertices) if (!reader.readBytes(&vertex.position, sizeof(vertex.position)) || !reader.readBytes(&vertex.normal, sizeof(vertex.normal)) || !reader.readBytes(&vertex.uv, sizeof(vertex.uv)) || !reader.readBytes(&vertex.tangent, sizeof(vertex.tangent))) return cacheError("vertex data is truncated");
    for (auto& value : asset.meshletVertices) if (!reader.read32(value)) return cacheError("meshlet vertex data is truncated");
    if (!reader.readBytes(asset.meshletTriangles.data(), asset.meshletTriangles.size())) return cacheError("meshlet triangle data is truncated");
    for (auto& value : asset.indices) if (!reader.read32(value)) return cacheError("index data is truncated");
    for (auto& meshlet : asset.meshlets) if (!reader.read32(meshlet.vertexOffset) || !reader.read32(meshlet.vertexCount) || !reader.read32(meshlet.triangleOffset) || !reader.read32(meshlet.triangleCount) || !reader.read32(meshlet.indexOffset) || !reader.read32(meshlet.indexCount) || !reader.read32(meshlet.primitiveIndex) || !reader.read32(meshlet.lodIndex) || !reader.readBytes(&meshlet.sphere, sizeof(meshlet.sphere)) || !reader.readBytes(&meshlet.cone, sizeof(meshlet.cone)) || !reader.readFloat(meshlet.geometricError)) return cacheError("meshlet table is truncated");
    for (auto& lod : asset.lods) if (!reader.read32(lod.primitiveIndex) || !reader.read32(lod.meshletOffset) || !reader.read32(lod.meshletCount) || !reader.read32(lod.indexOffset) || !reader.read32(lod.indexCount) || !reader.readFloat(lod.geometricError) || !reader.readFloat(lod.ratio)) return cacheError("LOD table is truncated");
    for (auto& primitive : asset.primitives) if (!reader.read32(primitive.vertexOffset) || !reader.read32(primitive.vertexCount) || !reader.read32(primitive.materialIndex) || !reader.readBytes(&primitive.boundsMin, sizeof(primitive.boundsMin)) || !reader.readBytes(&primitive.boundsMax, sizeof(primitive.boundsMax))) return cacheError("primitive table is truncated");
    if (reader.offset != payload.size()) return cacheError("cache payload has trailing bytes");
    const auto inRange = [](std::uint32_t offset, std::uint32_t count, std::size_t size) { return static_cast<std::size_t>(offset) <= size && static_cast<std::size_t>(count) <= size - offset; };
    for (const auto& meshlet : asset.meshlets) if (meshlet.vertexCount > maxVertices || meshlet.triangleCount > maxTriangles || !inRange(meshlet.vertexOffset, meshlet.vertexCount, asset.meshletVertices.size()) || meshlet.triangleCount > std::numeric_limits<std::uint32_t>::max() / 3u || !inRange(meshlet.triangleOffset, meshlet.triangleCount * 3u, asset.meshletTriangles.size()) || !inRange(meshlet.indexOffset, meshlet.indexCount, asset.indices.size()) || meshlet.primitiveIndex >= asset.primitives.size() || meshlet.lodIndex >= asset.lods.size() || !std::isfinite(meshlet.geometricError) || meshlet.geometricError < 0.0f) return cacheError("meshlet reference is out of range");
    for (const auto& lod : asset.lods) if (!inRange(lod.meshletOffset, lod.meshletCount, asset.meshlets.size()) || !inRange(lod.indexOffset, lod.indexCount, asset.indices.size()) || lod.primitiveIndex >= asset.primitives.size() || !std::isfinite(lod.geometricError) || lod.geometricError < 0.0f || !std::isfinite(lod.ratio) || lod.ratio < 0.0f || lod.ratio > 1.0f) return cacheError("LOD reference is out of range");
    for (const auto index : asset.indices) if (index >= asset.vertices.size()) return cacheError("index references missing vertex");
    if (metadata) { metadata->build.maxVertices = maxVertices; metadata->build.maxTriangles = maxTriangles; metadata->compressionLevel = level; }
    return Halcyon::Result<VirtualGeometryAsset>::success(std::move(asset));
}

} // namespace Halcyon::Renderer::Scene
