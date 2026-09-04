#include "SandboxExample.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <memory>

namespace Halcyon::Sandbox
{
namespace
{

struct State
{
    Entity modelEntity{};
};

} // namespace

ApplicationCallbacks makeCallbacks()
{
    auto state = std::make_shared<State>();
    ApplicationCallbacks callbacks;
    callbacks.onInitialize = [state](Engine& engine) -> Result<void>
    {
        Perspective perspective{};
        perspective.verticalFovRadians = glm::radians(55.0f);
        perspective.nearPlane = 0.1f;
        perspective.farPlane = 100.0f;
        auto& view = engine.defaultView();
        auto result = view.setPerspective(perspective);
        if (!result)
        {
            return result;
        }
        result = view.setViewport(view.viewport());
        if (!result)
        {
            return result;
        }
        result = view.lookAt({0.0f, 0.15f, 3.2f}, {0.0f, 0.0f, 0.0f});
        if (!result)
        {
            return result;
        }

        const SceneInstanceHandle instance = engine.sceneManager().findInstance("main");
        state->modelEntity = engine.sceneManager().rootEntity(instance);
        if (engine.scene().transforms().get(state->modelEntity) == nullptr)
        {
            return Result<void>::failure(MakeError(ErrorCode::NotFound,
                "configured sandbox scene instance is unavailable",
                "Sandbox"));
        }
        return Result<void>::success();
    };
    callbacks.onFrame = [state](Engine& engine, const FrameInfo& frame) -> Result<void>
    {
        auto* transform = engine.scene().transforms().get(state->modelEntity);
        if (transform == nullptr)
        {
            return Result<void>::failure(
                MakeError(ErrorCode::InvalidState, "model transform is unavailable", "Sandbox"));
        }
        const float angle = static_cast<float>(frame.elapsedSeconds) * glm::radians(30.0f);
        transform->localTransform =
            glm::rotate(glm::mat4{1.0f}, angle, glm::vec3{0.0f, 1.0f, 0.0f});
        transform->dirty = true;
        return Result<void>::success();
    };
    callbacks.onShutdown = [state](Engine&)
    {
        state->modelEntity = Entity::invalid();
    };
    return callbacks;
}

} // namespace Halcyon::Sandbox
