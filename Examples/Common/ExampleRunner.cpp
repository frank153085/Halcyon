#include "ExampleRunner.h"

#include "Halcyon/Application.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <memory>

namespace Halcyon::Examples
{
namespace
{

struct State
{
    Entity modelEntity{};
    std::string instanceName;
};

ApplicationCallbacks makeCallbacks(
    const std::shared_ptr<State>& state, const ExampleDefinition& definition)
{
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

        const SceneInstanceHandle instance =
            engine.sceneManager().findInstance(state->instanceName);
        state->modelEntity = engine.sceneManager().rootEntity(instance);
        if (engine.scene().transforms().get(state->modelEntity) == nullptr)
        {
            return Result<void>::failure(MakeError(ErrorCode::NotFound,
                "configured scene instance is unavailable: " + state->instanceName,
                "Example"));
        }
        return Result<void>::success();
    };
    callbacks.onFrame = [state](Engine& engine, const FrameInfo& frame) -> Result<void>
    {
        auto* transform = engine.scene().transforms().get(state->modelEntity);
        if (transform == nullptr)
        {
            return Result<void>::failure(
                MakeError(ErrorCode::InvalidState, "model transform is unavailable", "Example"));
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
    if (definition.onInitialize)
    {
        callbacks.onInitialize = definition.onInitialize;
    }
    if (definition.onFrame)
    {
        callbacks.onFrame = definition.onFrame;
    }
    if (definition.onShutdown)
    {
        callbacks.onShutdown = definition.onShutdown;
    }
    return callbacks;
}

} // namespace

StaticScene makeTriangleScene()
{
    StaticScene scene;
    scene.sourcePath = "memory://halcyon/example-triangle";
    StaticSceneMaterial material;
    material.name = "TriangleMaterial";
    material.pbr.baseColor = {1.0f, 0.35f, 0.18f, 1.0f};
    material.pbr.roughness = 0.75f;
    scene.materials.push_back(material);

    StaticScenePrimitive primitive;
    primitive.vertices = {
        StaticSceneVertex{{0.0f, -0.72f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.5f, 1.0f}},
        StaticSceneVertex{{0.72f, 0.72f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
        StaticSceneVertex{{-0.72f, 0.72f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
    };
    primitive.indices = {0, 1, 2};
    primitive.boundsMin = {-0.72f, -0.72f, 0.0f};
    primitive.boundsMax = {0.72f, 0.72f, 0.0f};
    scene.primitives.push_back(std::move(primitive));

    StaticSceneNode node;
    node.name = "Triangle";
    node.primitiveIndices.push_back(0);
    scene.nodes.push_back(std::move(node));
    return scene;
}

int run(const ExampleDefinition& definition, int argc, char** argv)
{
    ApplicationConfig config;
    config.window.title = definition.title != nullptr ? definition.title : "Halcyon Example";
    config.engine.scene = definition.scene;
#ifdef HALCYON_ASSET_ROOT
    if (config.engine.scene.assetRoot.empty())
    {
        config.engine.scene.assetRoot = HALCYON_ASSET_ROOT;
    }
#endif

    auto state = std::make_shared<State>();
    state->instanceName = definition.animatedInstanceName;
    if (state->instanceName.empty() && !definition.scene.instances.empty())
    {
        state->instanceName = definition.scene.instances.front().name;
    }
    return Application::run(argc, argv, std::move(config), makeCallbacks(state, definition));
}

} // namespace Halcyon::Examples
