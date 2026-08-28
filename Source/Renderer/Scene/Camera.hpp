#pragma once

#include "Core/Result.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <limits>
#include <type_traits>

namespace Halcyon::Renderer::Scene
{

// Halcyon's fixed camera convention:
//   * right-handed world/view space
//   * +Y is up and an identity camera looks along -Z
//   * Vulkan NDC depth is [0, 1]
//   * reversed Z maps near to 1 and far/infinity to 0
//   * projection includes Vulkan's Y inversion
struct Perspective
{
    float verticalFovRadians = glm::radians(60.0f);
    float aspectRatio = 16.0f / 9.0f;
    float nearPlane = 0.1f;
    float farPlane = std::numeric_limits<float>::infinity();
};

struct ViewportExtent
{
    std::uint32_t width = 1280;
    std::uint32_t height = 720;

    [[nodiscard]] constexpr bool empty() const noexcept
    {
        return width == 0 || height == 0;
    }
};

// Column-major data ready for a std140/scalar-layout constant buffer.  Every
// member starts at a 16-byte boundary and the complete block is a multiple of
// 16 bytes.  A zero far value in positionAndFar.w denotes an infinite plane.
struct alignas(16) CameraData
{
    alignas(16) glm::mat4 view{1.0f};
    alignas(16) glm::mat4 projection{1.0f};
    alignas(16) glm::mat4 viewProjection{1.0f};
    alignas(16) glm::mat4 inverseViewProjection{1.0f};
    alignas(16) glm::vec4 positionAndNear{0.0f, 0.0f, 0.0f, 0.1f};
    alignas(16) glm::vec4 forwardAndFar{0.0f, 0.0f, -1.0f, 0.0f};
    // xy = pixel extent, zw = reciprocal pixel extent.
    alignas(16) glm::vec4 viewportAndInvViewport{1280.0f, 720.0f,
                                                  1.0f / 1280.0f,
                                                  1.0f / 720.0f};
};

static_assert(alignof(CameraData) == 16);
static_assert(sizeof(CameraData) % 16 == 0);
static_assert(std::is_standard_layout_v<CameraData>);

// Standalone constructors are useful in tests and non-interactive replay
// tools.  On failure they return InvalidArgument and never produce NaNs.
[[nodiscard]] Halcyon::Result<glm::mat4> makeReversedZProjection(
    const Perspective& perspective);

class Camera final
{
public:
    Camera() = default;

    [[nodiscard]] Halcyon::Result<void> setPerspective(
        const Perspective& perspective);
    [[nodiscard]] Halcyon::Result<void> setViewport(ViewportExtent extent);

    [[nodiscard]] Halcyon::Result<void> setPosition(const glm::vec3& position);
    [[nodiscard]] Halcyon::Result<void> setOrientation(
        const glm::quat& cameraToWorld);
    [[nodiscard]] Halcyon::Result<void> lookAt(
        const glm::vec3& position,
        const glm::vec3& target,
        const glm::vec3& upHint = glm::vec3{0.0f, 1.0f, 0.0f});

    [[nodiscard]] const glm::vec3& position() const noexcept { return position_; }
    [[nodiscard]] const glm::quat& orientation() const noexcept
    {
        return orientation_;
    }
    [[nodiscard]] const Perspective& perspective() const noexcept
    {
        return perspective_;
    }
    [[nodiscard]] ViewportExtent viewport() const noexcept { return viewport_; }

    [[nodiscard]] glm::vec3 forward() const noexcept;
    [[nodiscard]] glm::vec3 right() const noexcept;
    [[nodiscard]] glm::vec3 up() const noexcept;
    [[nodiscard]] glm::mat4 viewMatrix() const noexcept;
    [[nodiscard]] glm::mat4 projectionMatrix() const noexcept;
    [[nodiscard]] CameraData data() const noexcept;

private:
    glm::vec3 position_{0.0f};
    glm::quat orientation_{1.0f, 0.0f, 0.0f, 0.0f};
    Perspective perspective_{};
    ViewportExtent viewport_{};
};

} // namespace Halcyon::Renderer::Scene
