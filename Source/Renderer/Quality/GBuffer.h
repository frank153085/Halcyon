#pragma once

// Compact, deterministic G-buffer encoding used by the traditional deferred
// path.  Normals and motion are signed-normalized 16-bit pairs; colour and
// scalar material values use UNORM8.  The representation is API independent
// and can therefore be used for golden-image reference tests.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>

namespace Halcyon::Renderer::Quality
{

struct GBufferPixel
{
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    glm::vec4 albedo{1.0f};
    float metallic = 0.0f;
    float roughness = 1.0f;
    float ambientOcclusion = 1.0f;
    glm::vec3 emissive{0.0f};
    glm::vec2 motion{0.0f}; // UV delta from previous to current frame.
};

struct PackedGBuffer
{
    std::uint32_t normalOct = 0;
    std::uint32_t albedo = 0;
    std::uint32_t material = 0;
    std::uint32_t emissive = 0;
    std::uint32_t motion = 0;
};

[[nodiscard]] inline std::uint16_t packUnorm16(float value) noexcept
{
    if (!std::isfinite(value)) value = 0.0f;
    return static_cast<std::uint16_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 65535.0f));
}

[[nodiscard]] inline float unpackUnorm16(std::uint16_t value) noexcept
{
    return static_cast<float>(value) / 65535.0f;
}

[[nodiscard]] inline std::int16_t packSnorm16(float value) noexcept
{
    if (!std::isfinite(value)) value = 0.0f;
    return static_cast<std::int16_t>(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
}

[[nodiscard]] inline float unpackSnorm16(std::int16_t value) noexcept
{
    return std::max(-1.0f, static_cast<float>(value) / 32767.0f);
}

[[nodiscard]] inline glm::vec2 octEncode(const glm::vec3& value) noexcept
{
    glm::vec3 normal = glm::normalize(value);
    if (!std::isfinite(normal.x) || !std::isfinite(normal.y) || !std::isfinite(normal.z))
    {
        normal = glm::vec3{0.0f, 0.0f, 1.0f};
    }
    const float denominator = std::abs(normal.x) + std::abs(normal.y) + std::abs(normal.z);
    glm::vec2 encoded = glm::vec2{normal} / std::max(denominator, 1.0e-7f);
    if (normal.z < 0.0f)
    {
        const glm::vec2 signNotZero{
            encoded.x < 0.0f ? -1.0f : 1.0f, encoded.y < 0.0f ? -1.0f : 1.0f};
        encoded = (glm::vec2{1.0f} - glm::abs(glm::vec2{encoded.y, encoded.x})) * signNotZero;
    }
    return encoded;
}

[[nodiscard]] inline glm::vec3 octDecode(const glm::vec2& encoded) noexcept
{
    glm::vec3 normal{encoded.x, encoded.y, 1.0f - std::abs(encoded.x) - std::abs(encoded.y)};
    if (normal.z < 0.0f)
    {
        const float x = normal.x;
        const float y = normal.y;
        normal.x = (1.0f - std::abs(y)) * (x < 0.0f ? -1.0f : 1.0f);
        normal.y = (1.0f - std::abs(x)) * (y < 0.0f ? -1.0f : 1.0f);
    }
    const float length = glm::length(normal);
    return length > 1.0e-7f ? normal / length : glm::vec3{0.0f, 0.0f, 1.0f};
}

[[nodiscard]] inline glm::vec2 encodeNormalOctahedral(const glm::vec3& normal) noexcept
{
    return octEncode(normal);
}

[[nodiscard]] inline glm::vec3 decodeNormalOctahedral(const glm::vec2& encoded) noexcept
{
    return octDecode(encoded);
}

[[nodiscard]] inline std::uint32_t packRgba8(const glm::vec4& value) noexcept
{
    const auto channel = [](float component)
    {
        if (!std::isfinite(component)) component = 0.0f;
        return static_cast<std::uint32_t>(std::lround(std::clamp(component, 0.0f, 1.0f) * 255.0f));
    };
    return channel(value.x) | (channel(value.y) << 8u) | (channel(value.z) << 16u) |
           (channel(value.w) << 24u);
}

[[nodiscard]] inline glm::vec4 unpackRgba8(std::uint32_t value) noexcept
{
    return glm::vec4{static_cast<float>(value & 0xffu),
               static_cast<float>((value >> 8u) & 0xffu),
               static_cast<float>((value >> 16u) & 0xffu),
               static_cast<float>((value >> 24u) & 0xffu)} /
           255.0f;
}

[[nodiscard]] inline PackedGBuffer packGBuffer(const GBufferPixel& pixel) noexcept
{
    const glm::vec2 oct = octEncode(pixel.normal);
    const auto encodeSignedPair = [](const glm::vec2& value)
    {
        return static_cast<std::uint32_t>(static_cast<std::uint16_t>(packSnorm16(value.x))) |
               (static_cast<std::uint32_t>(static_cast<std::uint16_t>(packSnorm16(value.y)))
                   << 16u);
    };
    const auto encodeByte = [](float value)
    {
        if (!std::isfinite(value)) value = 0.0f;
        return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    PackedGBuffer packed{};
    packed.normalOct = encodeSignedPair(oct);
    packed.albedo = packRgba8(pixel.albedo);
    packed.material = encodeByte(pixel.metallic) | (encodeByte(pixel.roughness) << 8u) |
                      (encodeByte(pixel.ambientOcclusion) << 16u);
    packed.emissive = packRgba8(glm::vec4{pixel.emissive, 1.0f});
    packed.motion = encodeSignedPair(pixel.motion);
    return packed;
}

[[nodiscard]] inline GBufferPixel unpackGBuffer(const PackedGBuffer& packed) noexcept
{
    const auto decodeSignedPair = [](std::uint32_t value)
    {
        return glm::vec2{unpackSnorm16(static_cast<std::int16_t>(value & 0xffffu)),
            unpackSnorm16(static_cast<std::int16_t>(value >> 16u))};
    };
    GBufferPixel pixel{};
    pixel.normal = octDecode(decodeSignedPair(packed.normalOct));
    pixel.albedo = unpackRgba8(packed.albedo);
    pixel.metallic = static_cast<float>(packed.material & 0xffu) / 255.0f;
    pixel.roughness = static_cast<float>((packed.material >> 8u) & 0xffu) / 255.0f;
    pixel.ambientOcclusion = static_cast<float>((packed.material >> 16u) & 0xffu) / 255.0f;
    pixel.emissive = glm::vec3{unpackRgba8(packed.emissive)};
    pixel.motion = decodeSignedPair(packed.motion);
    return pixel;
}

[[nodiscard]] inline PackedGBuffer packGbuffer(const GBufferPixel& pixel) noexcept
{
    return packGBuffer(pixel);
}

[[nodiscard]] inline GBufferPixel unpackGbuffer(const PackedGBuffer& packed) noexcept
{
    return unpackGBuffer(packed);
}

} // namespace Halcyon::Renderer::Quality
