#pragma once

#include "Core/Result.h"
#include "RenderTypes.h"
#include "Renderer/Scene/Camera.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Halcyon
{

using Camera = Renderer::Scene::Camera;
using CameraData = Renderer::Scene::CameraData;
using Perspective = Renderer::Scene::Perspective;

class View final
{
public:
    View() = default;

    [[nodiscard]] Camera& camera() noexcept
    {
        return camera_;
    }
    [[nodiscard]] const Camera& camera() const noexcept
    {
        return camera_;
    }

    [[nodiscard]] Result<void> setPerspective(const Perspective& perspective);
    [[nodiscard]] Result<void> setViewport(Extent2D extent);
    [[nodiscard]] Result<void> setPosition(const glm::vec3& position);
    [[nodiscard]] Result<void> setOrientation(const glm::quat& cameraToWorld);
    [[nodiscard]] Result<void> lookAt(const glm::vec3& position,
        const glm::vec3& target,
        const glm::vec3& up = glm::vec3{0.0f, 1.0f, 0.0f});

    [[nodiscard]] Extent2D viewport() const noexcept
    {
        return viewport_;
    }

private:
    Camera camera_{};
    Extent2D viewport_{};
};

} // namespace Halcyon
