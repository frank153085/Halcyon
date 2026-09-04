#pragma once

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

namespace Halcyon::Renderer::Quality
{

struct TemporalAAConfig
{
    float historyWeight = 0.9f;
    float sharpen = 0.0f;
    bool clampHistory = true;
};

struct TemporalNeighborhood
{
    glm::vec3 minimum{0.0f};
    glm::vec3 maximum{1.0f};
};

struct TemporalSample
{
    glm::vec3 current{0.0f};
    glm::vec3 history{0.0f};
    glm::vec2 motion{0.0f};
    bool historyValid = false;
};

[[nodiscard]] inline glm::vec2 clipToUv(const glm::vec4& clip) noexcept
{
    if (std::abs(clip.w) < 1.0e-7f)
    {
        return glm::vec2{0.5f};
    }
    return glm::vec2{clip} / clip.w * 0.5f + 0.5f;
}

[[nodiscard]] inline glm::vec2 computeMotionVector(const glm::vec3& worldPosition,
    const glm::mat4& currentViewProjection,
    const glm::mat4& previousViewProjection) noexcept
{
    // Positive motion points from the previous pixel to the current pixel.
    return clipToUv(currentViewProjection * glm::vec4{worldPosition, 1.0f}) -
           clipToUv(previousViewProjection * glm::vec4{worldPosition, 1.0f});
}

[[nodiscard]] inline glm::vec2 reprojectUv(
    const glm::vec2& currentUv, const glm::vec2& motion) noexcept
{
    return glm::clamp(currentUv - motion, glm::vec2{0.0f}, glm::vec2{1.0f});
}

[[nodiscard]] inline glm::vec3 clampHistory(
    const glm::vec3& history, const TemporalNeighborhood& neighborhood) noexcept
{
    return glm::clamp(history,
        glm::min(neighborhood.minimum, neighborhood.maximum),
        glm::max(neighborhood.minimum, neighborhood.maximum));
}

[[nodiscard]] inline glm::vec3 resolveTemporal(const TemporalSample& sample,
    const TemporalNeighborhood& neighborhood = {},
    const TemporalAAConfig& config = {}) noexcept
{
    const glm::vec3 current = glm::max(sample.current, glm::vec3{0.0f});
    if (!sample.historyValid)
    {
        return current;
    }
    const float weight = std::clamp(config.historyWeight, 0.0f, 0.99f);
    const glm::vec3 history = config.clampHistory ? clampHistory(sample.history, neighborhood)
                                                  : glm::max(sample.history, glm::vec3{0.0f});
    glm::vec3 result = glm::mix(current, history, weight);
    if (config.sharpen > 0.0f)
    {
        result += (current - result) * std::clamp(config.sharpen, 0.0f, 1.0f);
    }
    return glm::max(result, glm::vec3{0.0f});
}

} // namespace Halcyon::Renderer::Quality
