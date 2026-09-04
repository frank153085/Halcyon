#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <vector>

namespace Halcyon::Renderer::Quality
{

struct CsmConfig
{
    std::uint32_t cascadeCount = 4;
    float nearPlane = 0.1f;
    float farPlane = 100.0f;
    float splitLambda = 0.75f; // 0 = uniform, 1 = logarithmic.
    std::uint32_t resolution = 2048;
    float depthBias = 0.0015f;
};

struct CascadeShadow
{
    glm::mat4 viewProjection{1.0f};
    glm::vec4 splitAndTexel{0.0f}; // near, far, 1/width, 1/height
    glm::vec3 center{0.0f};
    float radius = 0.0f;
};

[[nodiscard]] inline std::uint32_t selectCascade(
    float viewDepth, const std::vector<float>& splits) noexcept
{
    if (!std::isfinite(viewDepth) || splits.empty())
    {
        return 0;
    }
    for (std::size_t i = 0; i < splits.size(); ++i)
    {
        if (viewDepth <= splits[i])
        {
            return static_cast<std::uint32_t>(i);
        }
    }
    return static_cast<std::uint32_t>(splits.size() - 1u);
}

[[nodiscard]] inline std::vector<float> computeCascadeSplits(const CsmConfig& config)
{
    const std::uint32_t count = std::max(1u, config.cascadeCount);
    const float nearPlane =
        std::isfinite(config.nearPlane) ? std::max(1.0e-4f, config.nearPlane) : 0.1f;
    const float requestedFar = std::isfinite(config.farPlane) ? config.farPlane : 100.0f;
    const float farPlane = std::max(nearPlane + 1.0e-4f, requestedFar);
    const float lambda =
        std::isfinite(config.splitLambda) ? std::clamp(config.splitLambda, 0.0f, 1.0f) : 0.75f;
    std::vector<float> splits;
    splits.reserve(count);
    for (std::uint32_t i = 1; i <= count; ++i)
    {
        const float fraction = static_cast<float>(i) / static_cast<float>(count);
        const float logarithmic = nearPlane * std::pow(farPlane / nearPlane, fraction);
        const float uniform = nearPlane + (farPlane - nearPlane) * fraction;
        splits.push_back(uniform * (1.0f - lambda) + logarithmic * lambda);
    }
    // Protect consumers from floating-point drift at the final split.
    splits.back() = farPlane;
    return splits;
}

[[nodiscard]] inline glm::vec3 unprojectViewDepth(const glm::mat4& inverseViewProjection,
    const glm::mat4& projection,
    float ndcX,
    float ndcY,
    float viewDepth) noexcept
{
    const glm::vec4 projected = projection * glm::vec4{0.0f, 0.0f, -viewDepth, 1.0f};
    const float ndcZ = std::abs(projected.w) > 1.0e-7f ? projected.z / projected.w : 0.0f;
    const glm::vec4 world = inverseViewProjection * glm::vec4{ndcX, ndcY, ndcZ, 1.0f};
    return std::abs(world.w) > 1.0e-7f ? glm::vec3{world} / world.w : glm::vec3{world};
}

[[nodiscard]] inline std::vector<CascadeShadow> buildCascades(const CsmConfig& config,
    const glm::mat4& view,
    const glm::mat4& projection,
    const glm::vec3& directionalLightDirection) noexcept
{
    std::vector<CascadeShadow> cascades;
    const auto splits = computeCascadeSplits(config);
    cascades.reserve(splits.size());
    const glm::mat4 inverseViewProjection = glm::inverse(projection * view);
    glm::vec3 lightDirection = directionalLightDirection;
    if (glm::dot(lightDirection, lightDirection) < 1.0e-8f)
    {
        lightDirection = glm::vec3{0.0f, -1.0f, 0.0f};
    }
    lightDirection = glm::normalize(lightDirection);
    float previousSplit = std::max(1.0e-4f, config.nearPlane);
    for (const float split : splits)
    {
        std::array<glm::vec3, 8> corners{};
        std::size_t corner = 0;
        for (float depth : {previousSplit, split})
        {
            for (const float y : {-1.0f, 1.0f})
            {
                for (const float x : {-1.0f, 1.0f})
                {
                    corners[corner++] =
                        unprojectViewDepth(inverseViewProjection, projection, x, y, depth);
                }
            }
        }
        glm::vec3 center{0.0f};
        for (const glm::vec3& point : corners)
        {
            center += point;
        }
        center /= static_cast<float>(corners.size());
        float radius = 0.0f;
        for (const glm::vec3& point : corners)
        {
            radius = std::max(radius, glm::length(point - center));
        }
        radius = std::max(radius, 1.0e-3f);
        // Use a stable up vector even when the light points almost vertically.
        const glm::vec3 up = std::abs(glm::dot(lightDirection, glm::vec3{0, 1, 0})) > 0.98f
                                 ? glm::vec3{0, 0, 1}
                                 : glm::vec3{0, 1, 0};
        const glm::vec3 lightPosition = center - lightDirection * (radius * 2.0f);
        glm::mat4 lightView = glm::lookAt(lightPosition, center, up);
        float minZ = std::numeric_limits<float>::max();
        float maxZ = -std::numeric_limits<float>::max();
        for (const glm::vec3& point : corners)
        {
            const glm::vec4 lightSpace = lightView * glm::vec4{point, 1.0f};
            minZ = std::min(minZ, lightSpace.z);
            maxZ = std::max(maxZ, lightSpace.z);
        }
        const float depthPadding = std::max(1.0f, radius * 0.1f);
        minZ -= depthPadding;
        maxZ += depthPadding;
        // Snap the orthographic centre to a shadow texel to avoid shimmering.
        if (config.resolution != 0)
        {
            const float texel = (2.0f * radius) / static_cast<float>(config.resolution);
            glm::vec4 lightCenter = lightView * glm::vec4{center, 1.0f};
            lightCenter.x = std::floor(lightCenter.x / texel + 0.5f) * texel;
            lightCenter.y = std::floor(lightCenter.y / texel + 0.5f) * texel;
            center = glm::vec3{glm::inverse(lightView) *
                               glm::vec4{lightCenter.x, lightCenter.y, lightCenter.z, 1.0f}};
        }
        const glm::mat4 lightProjection =
            glm::ortho(-radius, radius, -radius, radius, -maxZ, -minZ);
        cascades.push_back(CascadeShadow{lightProjection * lightView,
            glm::vec4{previousSplit,
                split,
                config.resolution != 0 ? 1.0f / static_cast<float>(config.resolution) : 0.0f,
                config.resolution != 0 ? 1.0f / static_cast<float>(config.resolution) : 0.0f},
            center,
            radius});
        previousSplit = split;
    }
    return cascades;
}

[[nodiscard]] inline std::vector<CascadeShadow> computeCsmCascades(const CsmConfig& config,
    const glm::mat4& view,
    const glm::mat4& projection,
    const glm::vec3& directionalLightDirection) noexcept
{
    return buildCascades(config, view, projection, directionalLightDirection);
}

} // namespace Halcyon::Renderer::Quality
