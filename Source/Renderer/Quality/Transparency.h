#pragma once

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <vector>

namespace Halcyon::Renderer::Quality
{

struct TransparentFragment
{
    glm::vec4 color{0.0f}; // RGB is linear HDR, alpha is coverage.
    float depth = 0.0f;
};

[[nodiscard]] inline glm::vec4 alphaBlendOver(
    const glm::vec4& source, const glm::vec4& destination) noexcept
{
    const auto finite = [](float value) { return std::isfinite(value) ? value : 0.0f; };
    const float alpha = std::clamp(finite(source.a), 0.0f, 1.0f);
    const float destinationAlpha = std::clamp(finite(destination.a), 0.0f, 1.0f);
    return {finite(source.r) * alpha + finite(destination.r) * (1.0f - alpha),
        finite(source.g) * alpha + finite(destination.g) * (1.0f - alpha),
        finite(source.b) * alpha + finite(destination.b) * (1.0f - alpha),
        alpha + destinationAlpha * (1.0f - alpha)};
}

// Stable back-to-front ordering for the traditional forward transparency
// pass.  Equal depths retain submission order, which is important for golden
// images and avoids frame-to-frame flicker.
inline void sortBackToFront(std::vector<TransparentFragment>& fragments)
{
    std::stable_sort(fragments.begin(),
        fragments.end(),
        [](const TransparentFragment& lhs, const TransparentFragment& rhs)
        {
            const float lhsDepth = std::isfinite(lhs.depth) ? lhs.depth : 0.0f;
            const float rhsDepth = std::isfinite(rhs.depth) ? rhs.depth : 0.0f;
            return lhsDepth > rhsDepth;
        });
}

struct WeightedTransparencyAccumulator
{
    glm::vec3 weightedColor{0.0f};
    float revealage = 1.0f;

    void add(const TransparentFragment& fragment) noexcept
    {
        const float alpha = std::clamp(
            std::isfinite(fragment.color.a) ? fragment.color.a : 0.0f, 0.0f, 1.0f);
        // Weighting favours opaque and nearby fragments while remaining bounded
        // for HDR values.  This is the McGuire/Bavoil weighted blended OIT
        // approximation used when sorting is too expensive.
        const float depth = std::isfinite(fragment.depth) ? fragment.depth : 0.0f;
        const float depthWeight = std::clamp(1.0f - depth, 0.01f, 1.0f);
        const float weight = std::max(1.0e-2f, alpha * depthWeight);
        const glm::vec3 color{
            std::isfinite(fragment.color.r) ? fragment.color.r : 0.0f,
            std::isfinite(fragment.color.g) ? fragment.color.g : 0.0f,
            std::isfinite(fragment.color.b) ? fragment.color.b : 0.0f};
        weightedColor += color * weight;
        revealage *= (1.0f - alpha);
    }

    [[nodiscard]] glm::vec4 resolve() const noexcept
    {
        const float transmittance = std::clamp(1.0f - revealage, 0.0f, 1.0f);
        const float denominator = std::max(1.0e-5f, transmittance);
        return {weightedColor / denominator, transmittance};
    }
};

} // namespace Halcyon::Renderer::Quality
