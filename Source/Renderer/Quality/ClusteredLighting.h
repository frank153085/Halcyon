#pragma once

// CPU reference implementation of clustered light assignment.  The same
// indexing and logarithmic depth convention is shared by the compute shader,
// making this useful for deterministic tests and fallback devices.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include <limits>
#include <vector>

namespace Halcyon::Renderer::Quality
{

struct ClusterGrid
{
    std::uint32_t tilesX = 16;
    std::uint32_t tilesY = 9;
    std::uint32_t slicesZ = 24;
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;

    [[nodiscard]] bool valid() const noexcept
    {
        return tilesX != 0 && tilesY != 0 && slicesZ != 0 && std::isfinite(nearPlane) &&
               std::isfinite(farPlane) && nearPlane > 0.0f && farPlane > nearPlane;
    }
    [[nodiscard]] std::size_t clusterCount() const noexcept
    {
        return static_cast<std::size_t>(tilesX) * tilesY * slicesZ;
    }
};

struct ClusterLight
{
    glm::vec3 position{0.0f};
    float range = 1.0f;
    std::uint32_t lightIndex = 0;
    bool directional = false;
};

struct ClusterLightLists
{
    std::vector<std::vector<std::uint32_t>> lights;
    std::vector<std::uint32_t> directionalLights;
    std::uint32_t overflowCount = 0;

    void reset(std::size_t count)
    {
        lights.clear();
        lights.resize(count);
        directionalLights.clear();
        overflowCount = 0;
    }

    struct Range
    {
        std::uint32_t offset = 0;
        std::uint32_t count = 0;
    };

    [[nodiscard]] std::vector<Range> flatten(std::vector<std::uint32_t>& indices) const
    {
        std::vector<Range> ranges;
        ranges.reserve(lights.size());
        indices.clear();
        for (const auto& list : lights)
        {
            const auto offset = static_cast<std::uint32_t>(indices.size());
            indices.insert(indices.end(), list.begin(), list.end());
            ranges.push_back(Range{offset, static_cast<std::uint32_t>(list.size())});
        }
        return ranges;
    }
};

[[nodiscard]] inline std::uint32_t clusterDepthSlice(
    float viewDepth, const ClusterGrid& grid) noexcept
{
    if (!grid.valid() || !std::isfinite(viewDepth))
    {
        return 0;
    }
    const float depth = std::clamp(viewDepth, grid.nearPlane, grid.farPlane);
    const float logarithmic =
        std::log(depth / grid.nearPlane) / std::log(grid.farPlane / grid.nearPlane);
    const auto slice = static_cast<std::uint32_t>(logarithmic * static_cast<float>(grid.slicesZ));
    return std::min(slice, grid.slicesZ - 1u);
}

[[nodiscard]] inline std::size_t clusterIndex(std::uint32_t tileX,
    std::uint32_t tileY,
    std::uint32_t sliceZ,
    const ClusterGrid& grid) noexcept
{
    if (!grid.valid())
    {
        return 0;
    }
    tileX = std::min(tileX, grid.tilesX - 1u);
    tileY = std::min(tileY, grid.tilesY - 1u);
    sliceZ = std::min(sliceZ, grid.slicesZ - 1u);
    return (static_cast<std::size_t>(sliceZ) * grid.tilesY + tileY) * grid.tilesX + tileX;
}

[[nodiscard]] inline ClusterLightLists assignClusteredLights(const ClusterGrid& grid,
    const glm::mat4& view,
    const glm::mat4& projection,
    glm::uvec2 viewport,
    const std::vector<ClusterLight>& pointLights,
    std::size_t maxLightsPerCluster = 128)
{
    ClusterLightLists result;
    result.reset(grid.clusterCount());
    if (!grid.valid() || viewport.x == 0 || viewport.y == 0 || maxLightsPerCluster == 0)
    {
        return result;
    }

    const glm::mat4 viewProjection = projection * view;
    for (const ClusterLight& light : pointLights)
    {
        if (!std::isfinite(light.range) || light.range <= 0.0f)
        {
            if (light.directional)
            {
                result.directionalLights.push_back(light.lightIndex);
            }
            continue;
        }
        if (light.directional)
        {
            result.directionalLights.push_back(light.lightIndex);
            continue;
        }
        const glm::vec4 viewPosition = view * glm::vec4{light.position, 1.0f};
        const float depth = -viewPosition.z;
        if (!std::isfinite(depth) || depth + light.range < grid.nearPlane ||
            depth - light.range > grid.farPlane)
        {
            continue;
        }

        // Project the centre and two conservative points on each axis.  This
        // avoids assumptions about a particular projection's handedness while
        // remaining deterministic for perspective and orthographic cameras.
        const glm::vec4 clip = viewProjection * glm::vec4{light.position, 1.0f};
        if (std::abs(clip.w) < 1.0e-6f)
        {
            continue;
        }
        const glm::vec2 centre = glm::vec2{clip} / clip.w * 0.5f + 0.5f;
        auto project = [&](const glm::vec3& p) -> glm::vec2
        {
            const glm::vec4 c = viewProjection * glm::vec4{p, 1.0f};
            if (std::abs(c.w) < 1.0e-6f)
            {
                return centre;
            }
            return glm::vec2{c} / c.w * 0.5f + 0.5f;
        };
        const glm::vec2 radiusPixels =
            glm::abs(project(light.position + glm::vec3{light.range, 0, 0}) - centre) *
            glm::vec2{viewport};
        const glm::vec2 radiusY =
            glm::abs(project(light.position + glm::vec3{0, light.range, 0}) - centre) *
            glm::vec2{viewport};
        const glm::vec2 radius = glm::max(radiusPixels, radiusY);
        const glm::vec2 minPixel = centre * glm::vec2{viewport} - radius;
        const glm::vec2 maxPixel = centre * glm::vec2{viewport} + radius;
        const auto toTileX = [&](float pixel) -> std::uint32_t
        {
            const float normalized = pixel / static_cast<float>(viewport.x);
            const auto tile = static_cast<std::int32_t>(std::floor(normalized * grid.tilesX));
            return static_cast<std::uint32_t>(
                std::clamp(tile, 0, static_cast<std::int32_t>(grid.tilesX) - 1));
        };
        const auto toTileY = [&](float pixel) -> std::uint32_t
        {
            const float normalized = pixel / static_cast<float>(viewport.y);
            const auto tile = static_cast<std::int32_t>(std::floor(normalized * grid.tilesY));
            return static_cast<std::uint32_t>(
                std::clamp(tile, 0, static_cast<std::int32_t>(grid.tilesY) - 1));
        };
        const std::uint32_t minX = toTileX(minPixel.x);
        const std::uint32_t maxX = toTileX(maxPixel.x);
        const std::uint32_t minY = toTileY(minPixel.y);
        const std::uint32_t maxY = toTileY(maxPixel.y);
        const std::uint32_t minZ =
            clusterDepthSlice(std::max(grid.nearPlane, depth - light.range), grid);
        const std::uint32_t maxZ =
            clusterDepthSlice(std::min(grid.farPlane, depth + light.range), grid);
        for (std::uint32_t z = minZ; z <= maxZ; ++z)
        {
            for (std::uint32_t y = minY; y <= maxY; ++y)
            {
                for (std::uint32_t x = minX; x <= maxX; ++x)
                {
                    auto& list = result.lights[clusterIndex(x, y, z, grid)];
                    if (list.size() < maxLightsPerCluster)
                    {
                        list.push_back(light.lightIndex);
                    }
                    else
                    {
                        ++result.overflowCount;
                    }
                }
            }
        }
    }
    return result;
}

[[nodiscard]] inline ClusterLightLists buildClusterLightLists(const ClusterGrid& grid,
    const glm::mat4& view,
    const glm::mat4& projection,
    glm::uvec2 viewport,
    const std::vector<ClusterLight>& pointLights,
    std::size_t maxLightsPerCluster = 128)
{
    return assignClusteredLights(
        grid, view, projection, viewport, pointLights, maxLightsPerCluster);
}

} // namespace Halcyon::Renderer::Quality
