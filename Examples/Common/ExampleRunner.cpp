#include "ExampleRunner.h"

#include "Halcyon/Application.h"

#include <filesystem>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <memory>

namespace Halcyon::Examples
{
namespace
{

std::string resolveStartupPath(const char* path)
{
    if (path == nullptr || *path == '\0')
    {
        return {};
    }
    namespace fs = std::filesystem;
    std::error_code error;
    const fs::path relativePath(path);
    if (relativePath.is_absolute() || fs::exists(relativePath, error))
    {
        return relativePath.string();
    }
#ifdef HALCYON_EXAMPLE_SOURCE_DIR
    const fs::path sourcePath = fs::path(HALCYON_EXAMPLE_SOURCE_DIR) / relativePath;
    error.clear();
    if (fs::exists(sourcePath, error))
    {
        return sourcePath.string();
    }
#endif
    return relativePath.string();
}

struct State
{
    Entity modelEntity{};
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

        state->modelEntity = engine.scene().createEntity();
        (void)engine.scene().transforms().add(state->modelEntity);
        (void)engine.scene().renderables().add(state->modelEntity);
        if (engine.scene().transforms().get(state->modelEntity) == nullptr)
        {
            return Result<void>::failure(
                MakeError(ErrorCode::Backend, "failed to create model transform", "Example"));
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
    callbacks.onShutdown = [state](Engine& engine)
    {
        if (state->modelEntity.isValid())
        {
            engine.scene().destroyEntity(state->modelEntity);
            state->modelEntity = Entity::invalid();
        }
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

int run(const ExampleDefinition& definition, int argc, char** argv)
{
    ApplicationConfig config;
    config.window.title = definition.title != nullptr ? definition.title : "Halcyon Example";
    const std::string startupTexture = resolveStartupPath(definition.startupTexturePath);
    const std::string startupMesh = resolveStartupPath(definition.startupMeshPath);
    config.engine.startupTexturePath = startupTexture.empty() ? nullptr : startupTexture.c_str();
    config.engine.startupMeshPath = startupMesh.empty() ? nullptr : startupMesh.c_str();

    auto state = std::make_shared<State>();
    return Application::run(argc, argv, std::move(config), makeCallbacks(state, definition));
}

} // namespace Halcyon::Examples
