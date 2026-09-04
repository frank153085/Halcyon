#pragma once

// Backend-neutral metallic/roughness material evaluation.  The functions in
// this header are also used by the reference path and by image-test tools, so
// they intentionally have no Vulkan dependencies.

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

namespace Halcyon::Renderer::Quality
{

inline constexpr float kPi = 3.14159265358979323846f;

struct PbrMaterial
{
    glm::vec4 baseColor{1.0f};
    float metallic = 0.0f;
    float roughness = 1.0f;
    glm::vec3 emissive{0.0f};
    float ambientOcclusion = 1.0f;
};

struct PbrLight
{
    glm::vec3 direction{0.0f, -1.0f, 0.0f}; // Direction from the surface to light.
    glm::vec3 color{1.0f};
    float intensity = 1.0f;
};

struct PbrInput
{
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    glm::vec3 viewDirection{0.0f, 0.0f, 1.0f};
    glm::vec3 lightDirection{0.0f, 0.0f, 1.0f};
    PbrMaterial material{};
};

[[nodiscard]] inline float saturate(float value) noexcept
{
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
}

[[nodiscard]] inline glm::vec3 saturate(const glm::vec3& value) noexcept
{
    return {saturate(value.x), saturate(value.y), saturate(value.z)};
}

[[nodiscard]] inline glm::vec3 safeNormalize(
    const glm::vec3& value, const glm::vec3& fallback = glm::vec3{0.0f, 0.0f, 1.0f}) noexcept
{
    const float lengthSquared = glm::dot(value, value);
    return std::isfinite(lengthSquared) && lengthSquared > 1.0e-12f
               ? value / std::sqrt(lengthSquared)
               : fallback;
}

[[nodiscard]] inline float distributionGGX(float nDotH, float roughness) noexcept
{
    const float safeRoughness = saturate(roughness);
    const float alpha = std::max(0.045f, safeRoughness * safeRoughness);
    const float alphaSquared = alpha * alpha;
    const float d = nDotH * nDotH * (alphaSquared - 1.0f) + 1.0f;
    return alphaSquared / std::max(kPi * d * d, 1.0e-7f);
}

[[nodiscard]] inline float geometrySchlickGGX(float nDotV, float roughness) noexcept
{
    const float safeRoughness = saturate(roughness);
    const float k = ((safeRoughness + 1.0f) * (safeRoughness + 1.0f)) * 0.125f;
    return nDotV / std::max(nDotV * (1.0f - k) + k, 1.0e-7f);
}

[[nodiscard]] inline float geometrySmith(float nDotV, float nDotL, float roughness) noexcept
{
    return geometrySchlickGGX(saturate(nDotV), roughness) *
           geometrySchlickGGX(saturate(nDotL), roughness);
}

[[nodiscard]] inline glm::vec3 fresnelSchlick(float cosTheta, const glm::vec3& f0) noexcept
{
    const float factor = std::pow(1.0f - saturate(cosTheta), 5.0f);
    return f0 + (glm::vec3{1.0f} - f0) * factor;
}

[[nodiscard]] inline glm::vec3 fresnelSchlickRoughness(
    float cosTheta, const glm::vec3& f0, float roughness) noexcept
{
    const glm::vec3 maximum = glm::max(glm::vec3{1.0f - saturate(roughness)}, f0);
    return f0 + (maximum - f0) * std::pow(1.0f - saturate(cosTheta), 5.0f);
}

[[nodiscard]] inline glm::vec3 evaluateDirectPbr(
    const PbrInput& input, const PbrLight& light) noexcept
{
    const glm::vec3 n = safeNormalize(input.normal);
    const glm::vec3 v = safeNormalize(input.viewDirection);
    const glm::vec3 l = safeNormalize(input.lightDirection);
    const glm::vec3 h = safeNormalize(v + l);
    const float nDotV = saturate(glm::dot(n, v));
    const float nDotL = saturate(glm::dot(n, l));
    const float nDotH = saturate(glm::dot(n, h));
    const float vDotH = saturate(glm::dot(v, h));
    if (nDotL <= 0.0f || nDotV <= 0.0f)
    {
        return input.material.emissive;
    }

    const glm::vec3 albedo = saturate(glm::vec3{input.material.baseColor});
    const float metallic = saturate(input.material.metallic);
    const float roughness = saturate(input.material.roughness);
    const glm::vec3 f0 = glm::mix(glm::vec3{0.04f}, albedo, metallic);
    const glm::vec3 f = fresnelSchlick(vDotH, f0);
    const float d = distributionGGX(nDotH, roughness);
    const float g = geometrySmith(nDotV, nDotL, roughness);
    const glm::vec3 specular = (d * g) * f / std::max(4.0f * nDotV * nDotL, 1.0e-6f);
    const glm::vec3 kd = (glm::vec3{1.0f} - f) * (1.0f - metallic);
    const glm::vec3 diffuse = kd * albedo / kPi;
    const glm::vec3 radiance = light.color * std::max(0.0f, light.intensity);
    return input.material.emissive + (diffuse + specular) * radiance * nDotL;
}

[[nodiscard]] inline glm::vec3 evaluateMetallicRoughness(
    const PbrInput& input, const PbrLight& light) noexcept
{
    return evaluateDirectPbr(input, light);
}

} // namespace Halcyon::Renderer::Quality
