#include "Halcyon/View.h"

namespace Halcyon
{

Result<void> View::setPerspective(const Perspective& perspective)
{
    return camera_.setPerspective(perspective);
}

Result<void> View::setViewport(Extent2D extent)
{
    const auto result =
        camera_.setViewport(Renderer::Scene::ViewportExtent{extent.width, extent.height});
    if (result)
    {
        viewport_ = extent;
    }
    return result;
}

Result<void> View::setPosition(const glm::vec3& position)
{
    return camera_.setPosition(position);
}

Result<void> View::setOrientation(const glm::quat& cameraToWorld)
{
    return camera_.setOrientation(cameraToWorld);
}

Result<void> View::lookAt(const glm::vec3& position, const glm::vec3& target, const glm::vec3& up)
{
    return camera_.lookAt(position, target, up);
}

} // namespace Halcyon
