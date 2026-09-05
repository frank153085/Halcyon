#include "M3Demo.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <memory>

namespace Halcyon::M3Demo
{
namespace
{
struct State
{
    Entity model{};
    std::string scene;
};
} // namespace

ApplicationCallbacks makeCallbacks(const std::string& sceneName)
{
    auto state = std::make_shared<State>();
    state->scene = sceneName;
    ApplicationCallbacks callbacks;
    callbacks.onInitialize = [state](Engine& engine) -> Result<void>
    {
        Perspective perspective{};
        perspective.verticalFovRadians = glm::radians(state->scene == "sponza" ? 52.0f : 55.0f);
        perspective.nearPlane = 0.05f;
        perspective.farPlane = state->scene == "sponza" ? 300.0f : 100.0f;
        auto result = engine.defaultView().setPerspective(perspective);
        if (!result)
        {
            return result;
        }
        result = engine.defaultView().setViewport(engine.defaultView().viewport());
        if (!result)
        {
            return result;
        }
        result = state->scene == "sponza"
                     // The Khronos Sponza asset is authored in a roughly
                     // 30x12x20 metre envelope after its root 0.008 scale.
                     // Start inside the courtyard so the first frame is a
                     // useful, deterministic view instead of an exterior
                     // wall close-up.
                     ? engine.defaultView().lookAt({0.0f, 2.5f, 0.0f}, {0.0f, 2.5f, -1.0f})
                     : engine.defaultView().lookAt({0.0f, 0.1f, 2.6f}, {0.0f, 0.0f, 0.0f});
        if (!result)
        {
            return result;
        }
        const SceneInstanceHandle instance = engine.sceneManager().findInstance("main");
        state->model = engine.sceneManager().rootEntity(instance);
        if (!state->model.isValid())
        {
            return Result<void>::failure(MakeError(
                ErrorCode::NotFound, "configured M3 scene instance is unavailable", "M3Demo"));
        }
        // A deterministic sun and fill light keep downloaded scenes visibly
        // lit even when no environment file is present.
        const Entity sun = engine.scene().createEntity();
        LightComponent sunLight{};
        sunLight.type = LightType::Directional;
        sunLight.color = {1.0f, 0.93f, 0.82f};
        sunLight.intensity = state->scene == "sponza" ? 0.65f : 2.0f;
        sunLight.range = 1000.0f;
        (void)engine.scene().lights().add(sun, sunLight);
        const Entity fill = engine.scene().createEntity();
        LightComponent fillLight{};
        fillLight.type = LightType::Point;
        fillLight.position = {2.0f, 2.5f, 2.0f};
        fillLight.color = {0.35f, 0.45f, 1.0f};
        fillLight.intensity = state->scene == "sponza" ? 1.5f : 8.0f;
        fillLight.range = 8.0f;
        (void)engine.scene().lights().add(fill, fillLight);
        return Result<void>::success();
    };
    callbacks.onFrame = [state](Engine& engine, const FrameInfo& frame) -> Result<void>
    {
        if (state->scene == "sponza")
        {
            return Result<void>::success();
        }
        if (state->scene == "stress")
        {
            // Deterministic camera trajectory used by the M4 visibility audit:
            // a slow orbit crosses occluder boundaries without introducing
            // input-dependent results in scripted runs.
            const float angle = static_cast<float>(frame.elapsedSeconds) * glm::radians(9.0f);
            const glm::vec3 position{std::sin(angle) * 34.0f, 22.0f,
                std::cos(angle) * 34.0f};
            const auto cameraResult = engine.defaultView().lookAt(position, {0.0f, 0.0f, 0.0f});
            if (!cameraResult) return cameraResult;
            return Result<void>::success();
        }
        auto* transform = engine.scene().transforms().get(state->model);
        if (transform == nullptr)
        {
            return Result<void>::failure(
                MakeError(ErrorCode::InvalidState, "M3 model transform is unavailable", "M3Demo"));
        }
        // Keep the camera and exposure deterministic by default.  A very slow
        // rigid rotation still exercises motion vectors and TAA when enabled.
        const float angle = static_cast<float>(frame.elapsedSeconds) * glm::radians(4.0f);
        transform->localTransform = glm::rotate(glm::mat4{1.0f}, angle, {0.0f, 1.0f, 0.0f});
        transform->dirty = true;
        return Result<void>::success();
    };
    callbacks.onShutdown = [state](Engine&)
    {
        state->model = Entity::invalid();
    };
    return callbacks;
}

} // namespace Halcyon::M3Demo
