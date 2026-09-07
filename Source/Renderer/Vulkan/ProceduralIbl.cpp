#include "FramePassContext.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>
#include <vulkan/vulkan.h>

namespace Halcyon::Vulkan
{
namespace
{
[[nodiscard]] std::uint16_t floatToHalf(float value) noexcept
{
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const std::uint32_t sign = (bits >> 16u) & 0x8000u;
    const std::uint32_t exponent = (bits >> 23u) & 0xffu;
    std::uint32_t mantissa = bits & 0x7fffffu;
    if (exponent == 0xffu)
    {
        return static_cast<std::uint16_t>(sign | (mantissa == 0u ? 0x7c00u : 0x7e00u));
    }
    const int halfExponent = static_cast<int>(exponent) - 127 + 15;
    if (halfExponent >= 31)
    {
        return static_cast<std::uint16_t>(sign | 0x7c00u);
    }
    if (halfExponent <= 0)
    {
        if (halfExponent < -10)
        {
            return static_cast<std::uint16_t>(sign);
        }
        mantissa |= 0x800000u;
        const std::uint32_t shift = static_cast<std::uint32_t>(14 - halfExponent);
        const std::uint32_t rounded = (mantissa + (1u << (shift - 1u))) >> shift;
        return static_cast<std::uint16_t>(sign | rounded);
    }
    mantissa += 0x1000u;
    if ((mantissa & 0x800000u) != 0u)
    {
        mantissa = 0;
        if (halfExponent + 1 >= 31)
            return static_cast<std::uint16_t>(sign | 0x7c00u);
        return static_cast<std::uint16_t>(sign |
            (static_cast<std::uint32_t>(halfExponent + 1) << 10u));
    }
    return static_cast<std::uint16_t>(sign |
        (static_cast<std::uint32_t>(halfExponent) << 10u) | (mantissa >> 13u));
}

[[nodiscard]] glm::vec3 cubeDirection(std::uint32_t face, float u, float v) noexcept
{
    const glm::vec3 direction = face == 0u ? glm::vec3{1.0f, -v, -u}
        : face == 1u ? glm::vec3{-1.0f, -v, u}
        : face == 2u ? glm::vec3{u, 1.0f, v}
        : face == 3u ? glm::vec3{u, -1.0f, -v}
        : face == 4u ? glm::vec3{u, -v, 1.0f}
                     : glm::vec3{-u, -v, -1.0f};
    return glm::normalize(direction);
}

[[nodiscard]] glm::vec3 proceduralSky(glm::vec3 direction, float roughness) noexcept
{
    const float skyAmount = glm::smoothstep(-0.08f, 0.18f, direction.y);
    const float zenith = std::pow(std::max(direction.y, 0.0f), 0.35f);
    const glm::vec3 ground{0.04f, 0.037f, 0.032f};
    const glm::vec3 horizon{0.28f, 0.26f, 0.23f};
    const glm::vec3 top{0.14f, 0.15f, 0.17f};
    glm::vec3 radiance = glm::mix(ground, glm::mix(horizon, top, zenith), skyAmount);
    const glm::vec3 sunDirection = glm::normalize(glm::vec3{0.32f, 0.88f, 0.24f});
    const float sunExponent = glm::mix(768.0f, 4.0f, roughness);
    const float sun = std::pow(std::max(glm::dot(direction, sunDirection), 0.0f), sunExponent);
    radiance += glm::vec3{6.5f, 5.6f, 4.6f} * sun * (1.0f - roughness);
    const glm::vec3 average{0.12f, 0.11f, 0.10f};
    return glm::mix(radiance, average, roughness * roughness * 0.82f);
}

void appendRgba16(std::vector<std::uint16_t>& output, const glm::vec3& color)
{
    output.push_back(floatToHalf(color.r));
    output.push_back(floatToHalf(color.g));
    output.push_back(floatToHalf(color.b));
    output.push_back(floatToHalf(1.0f));
}

[[nodiscard]] float radicalInverse(std::uint32_t bits) noexcept
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xaaaaaaaau) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xccccccccu) >> 2u);
    bits = ((bits & 0x0f0f0f0fu) << 4u) | ((bits & 0xf0f0f0f0u) >> 4u);
    bits = ((bits & 0x00ff00ffu) << 8u) | ((bits & 0xff00ff00u) >> 8u);
    return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

[[nodiscard]] glm::vec2 integrateBrdf(float nDotV, float roughness) noexcept
{
    constexpr std::uint32_t sampleCount = 64u;
    constexpr float pi = 3.14159265358979323846f;
    const glm::vec3 view{std::sqrt(std::max(0.0f, 1.0f - nDotV * nDotV)), 0.0f, nDotV};
    float scale = 0.0f;
    float bias = 0.0f;
    const float alpha = std::max(0.002f, roughness * roughness);
    for (std::uint32_t sample = 0; sample < sampleCount; ++sample)
    {
        const float xiX = static_cast<float>(sample) / static_cast<float>(sampleCount);
        const float xiY = radicalInverse(sample);
        const float phi = 2.0f * pi * xiX;
        const float cosTheta = std::sqrt((1.0f - xiY) /
            std::max(1.0f + (alpha * alpha - 1.0f) * xiY, 1.0e-6f));
        const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
        const glm::vec3 halfVector{std::cos(phi) * sinTheta, std::sin(phi) * sinTheta,
            cosTheta};
        const glm::vec3 light = glm::normalize(2.0f * glm::dot(view, halfVector) * halfVector - view);
        const float nDotL = std::max(light.z, 0.0f);
        const float nDotH = std::max(halfVector.z, 0.0f);
        const float vDotH = std::max(glm::dot(view, halfVector), 0.0f);
        if (nDotL <= 0.0f || nDotH <= 0.0f)
            continue;
        const float k = roughness * roughness * 0.5f;
        const float gV = nDotV / std::max(nDotV * (1.0f - k) + k, 1.0e-6f);
        const float gL = nDotL / std::max(nDotL * (1.0f - k) + k, 1.0e-6f);
        const float visibility = gV * gL * vDotH /
            std::max(nDotH * nDotV, 1.0e-6f);
        const float fresnel = std::pow(1.0f - vDotH, 5.0f);
        scale += (1.0f - fresnel) * visibility;
        bias += fresnel * visibility;
    }
    return {scale / static_cast<float>(sampleCount), bias / static_cast<float>(sampleCount)};
}

[[nodiscard]] ProceduralIblData createProceduralIblImpl()
{
    ProceduralIblData result;
    constexpr std::uint32_t irradianceSize = 32u;
    for (std::uint32_t face = 0; face < 6u; ++face)
    {
        VkBufferImageCopy copy{};
        copy.bufferOffset = result.irradiance.size() * sizeof(std::uint16_t);
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, face, 1};
        copy.imageExtent = {irradianceSize, irradianceSize, 1};
        result.irradianceCopies.push_back(copy);
        for (std::uint32_t y = 0; y < irradianceSize; ++y)
            for (std::uint32_t x = 0; x < irradianceSize; ++x)
            {
                const float u = (2.0f * (static_cast<float>(x) + 0.5f) / irradianceSize) - 1.0f;
                const float v = (2.0f * (static_cast<float>(y) + 0.5f) / irradianceSize) - 1.0f;
                const glm::vec3 direction = cubeDirection(face, u, v);
                const glm::vec3 diffuse = proceduralSky(direction, 0.82f) * 3.14159265f;
                appendRgba16(result.irradiance, diffuse);
            }
    }
    constexpr std::uint32_t baseSize = 64u;
    constexpr std::uint32_t mipCount = 7u;
    for (std::uint32_t mip = 0; mip < mipCount; ++mip)
    {
        const std::uint32_t size = std::max(1u, baseSize >> mip);
        const float roughness = static_cast<float>(mip) / static_cast<float>(mipCount - 1u);
        for (std::uint32_t face = 0; face < 6u; ++face)
        {
            VkBufferImageCopy copy{};
            copy.bufferOffset = result.prefiltered.size() * sizeof(std::uint16_t);
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, face, 1};
            copy.imageExtent = {size, size, 1};
            result.prefilteredCopies.push_back(copy);
            for (std::uint32_t y = 0; y < size; ++y)
                for (std::uint32_t x = 0; x < size; ++x)
                {
                    const float u = (2.0f * (static_cast<float>(x) + 0.5f) / size) - 1.0f;
                    const float v = (2.0f * (static_cast<float>(y) + 0.5f) / size) - 1.0f;
                    appendRgba16(result.prefiltered,
                        proceduralSky(cubeDirection(face, u, v), roughness));
                }
        }
    }
    constexpr std::uint32_t lutSize = 128u;
    VkBufferImageCopy lutCopy{};
    lutCopy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    lutCopy.imageExtent = {lutSize, lutSize, 1};
    result.brdfCopies.push_back(lutCopy);
    result.brdf.reserve(lutSize * lutSize * 2u);
    for (std::uint32_t y = 0; y < lutSize; ++y)
        for (std::uint32_t x = 0; x < lutSize; ++x)
        {
            const glm::vec2 integrated = integrateBrdf(
                (static_cast<float>(x) + 0.5f) / lutSize,
                (static_cast<float>(y) + 0.5f) / lutSize);
            result.brdf.push_back(floatToHalf(integrated.x));
            result.brdf.push_back(floatToHalf(integrated.y));
        }
    return result;
}

} // namespace

ProceduralIblData createProceduralIbl()
{
    return createProceduralIblImpl();
}

} // namespace Halcyon::Vulkan
