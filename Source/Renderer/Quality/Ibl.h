#pragma once

#include "Pbr.h"

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

namespace Halcyon::Renderer::Quality
{

// Values sampled from a precomputed environment map.  A renderer can fill
// these from cube-map descriptors; CPU tools may use the same contract with
// analytic or test environments.
struct IblSample
{
    glm::vec3 irradiance{0.0f};
    glm::vec3 prefilteredRadiance{0.0f};
    glm::vec2 brdf{1.0f, 0.0f};
};

struct IblSettings
{
    float intensity = 1.0f;
    float maxReflectionLod = 8.0f;
    bool enabled = true;
};

[[nodiscard]] inline glm::vec3 evaluateIbl(const PbrMaterial& material,
    const glm::vec3& normal,
    const glm::vec3& viewDirection,
    const IblSample& sample,
    const IblSettings& settings = {}) noexcept
{
    if (!settings.enabled)
    {
        return material.emissive;
    }
    const glm::vec3 n = safeNormalize(normal);
    const glm::vec3 v = safeNormalize(viewDirection);
    const float nDotV = saturate(glm::dot(n, v));
    const float metallic = saturate(material.metallic);
    const float roughness = saturate(material.roughness);
    const glm::vec3 albedo = saturate(glm::vec3{material.baseColor});
    const glm::vec3 f0 = glm::mix(glm::vec3{0.04f}, albedo, metallic);
    const glm::vec3 f = fresnelSchlickRoughness(nDotV, f0, roughness);
    // The BRDF integration LUT stores the scale and bias for the split-sum
    // approximation.  Clamp malformed LUT values so a bad asset cannot
    // introduce NaNs into HDR accumulation.
    const glm::vec2 brdf{std::isfinite(sample.brdf.x) ? std::max(0.0f, sample.brdf.x) : 0.0f,
        std::isfinite(sample.brdf.y) ? std::max(0.0f, sample.brdf.y) : 0.0f};
    const auto nonNegative = [](const glm::vec3& value)
    {
        return glm::vec3{std::isfinite(value.x) ? std::max(0.0f, value.x) : 0.0f,
            std::isfinite(value.y) ? std::max(0.0f, value.y) : 0.0f,
            std::isfinite(value.z) ? std::max(0.0f, value.z) : 0.0f};
    };
    const glm::vec3 irradiance = nonNegative(sample.irradiance);
    const glm::vec3 prefiltered = nonNegative(sample.prefilteredRadiance);
    const glm::vec3 specular = prefiltered * (f * brdf.x + brdf.y);
    const float ao = saturate(material.ambientOcclusion);
    return material.emissive + (irradiance * albedo * (1.0f - metallic) / kPi + specular) *
                                   (std::max(0.0f, settings.intensity) * ao);
}

[[nodiscard]] inline float reflectionLod(float roughness, const IblSettings& settings = {}) noexcept
{
    return saturate(roughness) * std::max(0.0f, settings.maxReflectionLod);
}

} // namespace Halcyon::Renderer::Quality
