#pragma once

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

namespace Halcyon::Renderer::Quality
{

[[nodiscard]] inline glm::vec3 applyExposure(const glm::vec3& hdr, float exposureStops) noexcept
{
    if (!std::isfinite(exposureStops))
    {
        exposureStops = 0.0f;
    }
    const float scale = std::exp2(std::clamp(exposureStops, -32.0f, 32.0f));
    return glm::max(hdr, glm::vec3{0.0f}) * scale;
}

// ACES fitted curve from the Narkowicz approximation.  It is intentionally
// applied per channel so the result is deterministic across CPU and HLSL
// reference implementations.
[[nodiscard]] inline float acesTonemap(float value) noexcept
{
    value = std::max(0.0f, value);
    constexpr float a = 2.51f;
    constexpr float b = 0.03f;
    constexpr float c = 2.43f;
    constexpr float d = 0.59f;
    constexpr float e = 0.14f;
    return std::clamp(
        (value * (a * value + b)) / std::max(value * (c * value + d) + e, 1.0e-7f), 0.0f, 1.0f);
}

[[nodiscard]] inline glm::vec3 acesTonemap(const glm::vec3& hdr) noexcept
{
    return {acesTonemap(hdr.x), acesTonemap(hdr.y), acesTonemap(hdr.z)};
}

[[nodiscard]] inline float linearToSrgb(float value) noexcept
{
    value = std::max(0.0f, value);
    return value <= 0.0031308f ? 12.92f * value : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

[[nodiscard]] inline glm::vec3 linearToSrgb(const glm::vec3& value) noexcept
{
    return {linearToSrgb(value.x), linearToSrgb(value.y), linearToSrgb(value.z)};
}

[[nodiscard]] inline glm::vec4 hdrToSrgb(const glm::vec4& hdr, float exposureStops = 0.0f) noexcept
{
    const glm::vec3 mapped =
        linearToSrgb(acesTonemap(applyExposure(glm::vec3{hdr}, exposureStops)));
    return {mapped, std::clamp(hdr.w, 0.0f, 1.0f)};
}

[[nodiscard]] inline glm::vec3 toneMapACES(
    const glm::vec3& hdr, float exposureStops = 0.0f) noexcept
{
    return acesTonemap(applyExposure(hdr, exposureStops));
}

} // namespace Halcyon::Renderer::Quality
