#pragma once

#include "Sha256.h"
#include "VirtualGeometryPages.h"

#include <cstddef>
#include <vector>

namespace Halcyon::Renderer::Scene
{

inline constexpr std::uint32_t kVirtualGeometryCacheVersion = 5u;
inline constexpr std::uint32_t kVirtualGeometryCachePageSize = 128u * 1024u;

enum VirtualGeometryCachePageFlags : std::uint32_t
{
    VirtualGeometryCachePageNone = 0u,
    VirtualGeometryCachePagePinnedRoot = 1u << 0u,
};

struct VirtualGeometryCachePageEntry
{
    std::uint64_t fileOffset = 0;
    std::uint64_t rawOffset = 0;
    std::uint32_t compressedSize = 0;
    std::uint32_t rawSize = 0;
    std::uint32_t crc32 = 0;
    std::uint32_t flags = VirtualGeometryCachePageNone;
};

struct VirtualGeometryCacheOptions
{
    VirtualGeometryBuildOptions build{};
    std::uint32_t compressionLevel = 3u;
    std::uint32_t pageSize = kVirtualGeometryCachePageSize;
};

struct VirtualGeometryCacheMetadata
{
    Sha256Digest sourceHash{};
    VirtualGeometryCacheOptions options{};
    std::uint64_t rawPayloadSize = 0;
    std::uint32_t rootPageCount = 0;
    // Geometry byte streams are omitted. All traversal and address metadata
    // required before entering the render loop is resident here.
    VirtualGeometryAsset residentAsset;
    VirtualGeometryPageLayout pageLayout;
    std::vector<VirtualGeometryCachePageEntry> pages;
    std::vector<std::uint32_t> rootPageIndices;
};

[[nodiscard]] Halcyon::Result<void> writeVirtualGeometryCache(
    const std::filesystem::path& path,
    const VirtualGeometryAsset& asset,
    const Sha256Digest& sourceHash,
    const VirtualGeometryCacheOptions& options = {});

[[nodiscard]] Halcyon::Result<VirtualGeometryAsset> readVirtualGeometryCache(
    const std::filesystem::path& path,
    const Sha256Digest* expectedSourceHash = nullptr,
    VirtualGeometryCacheOptions* metadata = nullptr,
    const VirtualGeometryCacheOptions* expectedOptions = nullptr);

// These two entry points read only the fixed header/page directory or one
// independently compressed page. They are the disk-facing ABI used by the
// asynchronous runtime streamer; neither operation materializes the asset.
[[nodiscard]] Halcyon::Result<VirtualGeometryCacheMetadata>
readVirtualGeometryCacheMetadata(
    const std::filesystem::path& path,
    const Sha256Digest* expectedSourceHash = nullptr,
    const VirtualGeometryCacheOptions* expectedOptions = nullptr);

[[nodiscard]] Halcyon::Result<std::vector<std::byte>> readVirtualGeometryCachePage(
    const std::filesystem::path& path,
    const VirtualGeometryCacheMetadata& metadata,
    std::uint32_t pageIndex);

} // namespace Halcyon::Renderer::Scene
