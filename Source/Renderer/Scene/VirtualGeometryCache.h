#pragma once

#include "Sha256.h"
#include "VirtualGeometry.h"

namespace Halcyon::Renderer::Scene
{

inline constexpr std::uint32_t kVirtualGeometryCacheVersion = 4u;

struct VirtualGeometryCacheOptions
{
    VirtualGeometryBuildOptions build{};
    std::uint32_t compressionLevel = 3u;
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

} // namespace Halcyon::Renderer::Scene
