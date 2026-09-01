#include "Camera.h"

#include <cmath>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <string>
#include <utility>

namespace Halcyon::Renderer::Scene
{
namespace
{

[[nodiscard]] bool finite(float value) noexcept
{
    return std::isfinite(value);
}

[[nodiscard]] bool finite(const glm::vec3& value) noexcept
{
    return finite(value.x) && finite(value.y) && finite(value.z);
}

[[nodiscard]] bool finite(const glm::quat& value) noexcept
{
    return finite(value.w) && finite(value.x) && finite(value.y) && finite(value.z);
}

[[nodiscard]] Halcyon::Error invalidCameraArgument(std::string message)
{
    return Halcyon::Error{Halcyon::ErrorCode::InvalidArgument, std::move(message), "camera"};
}

} // namespace

Halcyon::Result<glm::mat4> makeReversedZProjection(const Perspective& perspective)
{
    constexpr float pi = glm::pi<float>();
    if (!finite(perspective.verticalFovRadians) || perspective.verticalFovRadians <= 0.0f ||
        perspective.verticalFovRadians >= pi)
    {
        return Halcyon::Result<glm::mat4>::failure(
            invalidCameraArgument("vertical FOV must be finite and between 0 and pi"));
    }
    if (!finite(perspective.aspectRatio) || perspective.aspectRatio <= 0.0f)
    {
        return Halcyon::Result<glm::mat4>::failure(
            invalidCameraArgument("aspect ratio must be finite and positive"));
    }
    if (!finite(perspective.nearPlane) || perspective.nearPlane <= 0.0f)
    {
        return Halcyon::Result<glm::mat4>::failure(
            invalidCameraArgument("near plane must be finite and positive"));
    }
    const bool infiniteFar = std::isinf(perspective.farPlane) && perspective.farPlane > 0.0f;
    if (!infiniteFar &&
        (!finite(perspective.farPlane) || perspective.farPlane <= perspective.nearPlane))
    {
        return Halcyon::Result<glm::mat4>::failure(
            invalidCameraArgument("far plane must be greater than near or +infinity"));
    }

    const float focalLength = 1.0f / std::tan(perspective.verticalFovRadians * 0.5f);
    glm::mat4 projection{0.0f};
    projection[0][0] = focalLength / perspective.aspectRatio;
    // Flip Y once in the projection so all Vulkan viewports can use positive
    // height and the rest of the engine retains +Y-up coordinates.
    projection[1][1] = -focalLength;
    projection[2][3] = -1.0f;
    if (infiniteFar)
    {
        projection[2][2] = 0.0f;
        projection[3][2] = perspective.nearPlane;
    }
    else
    {
        const float inverseRange = 1.0f / (perspective.farPlane - perspective.nearPlane);
        projection[2][2] = perspective.nearPlane * inverseRange;
        projection[3][2] = perspective.nearPlane * perspective.farPlane * inverseRange;
    }
    return Halcyon::Result<glm::mat4>::success(projection);
}

Halcyon::Result<void> Camera::setPerspective(const Perspective& perspective)
{
    const auto projection = makeReversedZProjection(perspective);
    if (!projection)
    {
        return Halcyon::Result<void>::failure(projection.error());
    }
    perspective_ = perspective;
    return Halcyon::Result<void>::success();
}

Halcyon::Result<void> Camera::setViewport(ViewportExtent extent)
{
    if (extent.empty())
    {
        return Halcyon::Result<void>::failure(
            invalidCameraArgument("viewport extent must be non-zero"));
    }

    Perspective candidate = perspective_;
    candidate.aspectRatio = static_cast<float>(extent.width) / static_cast<float>(extent.height);
    const auto projection = makeReversedZProjection(candidate);
    if (!projection)
    {
        return Halcyon::Result<void>::failure(projection.error());
    }
    viewport_ = extent;
    perspective_ = candidate;
    return Halcyon::Result<void>::success();
}

Halcyon::Result<void> Camera::setPosition(const glm::vec3& position)
{
    if (!finite(position))
    {
        return Halcyon::Result<void>::failure(invalidCameraArgument("position must be finite"));
    }
    position_ = position;
    return Halcyon::Result<void>::success();
}

Halcyon::Result<void> Camera::setOrientation(const glm::quat& cameraToWorld)
{
    const float squaredLength = glm::dot(cameraToWorld, cameraToWorld);
    if (!finite(cameraToWorld) || !finite(squaredLength) || squaredLength <= 1.0e-12f)
    {
        return Halcyon::Result<void>::failure(
            invalidCameraArgument("orientation must be a finite, non-zero quaternion"));
    }
    orientation_ = glm::normalize(cameraToWorld);
    return Halcyon::Result<void>::success();
}

Halcyon::Result<void> Camera::lookAt(
    const glm::vec3& position, const glm::vec3& target, const glm::vec3& upHint)
{
    if (!finite(position) || !finite(target) || !finite(upHint))
    {
        return Halcyon::Result<void>::failure(
            invalidCameraArgument("look-at vectors must be finite"));
    }
    const glm::vec3 forwardDirection = target - position;
    if (glm::dot(forwardDirection, forwardDirection) <= 1.0e-12f)
    {
        return Halcyon::Result<void>::failure(
            invalidCameraArgument("look-at target must differ from position"));
    }

    const glm::vec3 normalizedForward = glm::normalize(forwardDirection);
    const glm::vec3 rightDirection = glm::cross(normalizedForward, upHint);
    if (glm::dot(rightDirection, rightDirection) <= 1.0e-12f)
    {
        return Halcyon::Result<void>::failure(
            invalidCameraArgument("look-at up vector must not be parallel to forward"));
    }
    const glm::vec3 normalizedRight = glm::normalize(rightDirection);
    const glm::vec3 normalizedUp = glm::normalize(glm::cross(normalizedRight, normalizedForward));

    // Columns of a camera-to-world rotation are local +X, +Y and +Z.
    const glm::mat3 cameraToWorld{normalizedRight, normalizedUp, -normalizedForward};
    const glm::quat candidate = glm::normalize(glm::quat_cast(cameraToWorld));
    position_ = position;
    orientation_ = candidate;
    return Halcyon::Result<void>::success();
}

glm::vec3 Camera::forward() const noexcept
{
    return orientation_ * glm::vec3{0.0f, 0.0f, -1.0f};
}

glm::vec3 Camera::right() const noexcept
{
    return orientation_ * glm::vec3{1.0f, 0.0f, 0.0f};
}

glm::vec3 Camera::up() const noexcept
{
    return orientation_ * glm::vec3{0.0f, 1.0f, 0.0f};
}

glm::mat4 Camera::viewMatrix() const noexcept
{
    const glm::mat4 worldToCameraRotation = glm::mat4_cast(glm::conjugate(orientation_));
    return glm::translate(worldToCameraRotation, -position_);
}

glm::mat4 Camera::projectionMatrix() const noexcept
{
    const auto projection = makeReversedZProjection(perspective_);
    // The Camera only stores validated/default Perspective values.  Returning
    // identity is a defensive fallback against memory corruption without
    // introducing exceptions into the render loop.
    return projection ? projection.value() : glm::mat4{1.0f};
}

CameraData Camera::data() const noexcept
{
    CameraData result{};
    result.view = viewMatrix();
    result.projection = projectionMatrix();
    result.viewProjection = result.projection * result.view;
    result.inverseViewProjection = glm::inverse(result.viewProjection);
    result.positionAndNear = glm::vec4{position_, perspective_.nearPlane};
    const float farOrZero = std::isinf(perspective_.farPlane) ? 0.0f : perspective_.farPlane;
    result.forwardAndFar = glm::vec4{forward(), farOrZero};
    const float width = static_cast<float>(viewport_.width);
    const float height = static_cast<float>(viewport_.height);
    result.viewportAndInvViewport = glm::vec4{width, height, 1.0f / width, 1.0f / height};
    return result;
}

} // namespace Halcyon::Renderer::Scene
