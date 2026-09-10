#include "ExampleRunner.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <memory>
#include <string>

int main(int argc, char** argv)
{
    std::string scene = "damaged-helmet";
    std::size_t instanceCount = 100'000;
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i] != nullptr ? argv[i] : "";
        if (argument == "--help" || argument == "-h")
        {
            return Halcyon::Application::run(argc, argv, {}, {});
        }
        if (argument == "--scene" && i + 1 < argc)
        {
            scene = argv[++i];
        }
        else if (argument.rfind("--scene=", 0) == 0)
        {
            scene = argument.substr(8);
        }
        else if (argument == "--instance-count" && i + 1 < argc)
        {
            instanceCount = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        }
        else if (argument.rfind("--instance-count=", 0) == 0)
        {
            instanceCount = static_cast<std::size_t>(std::strtoull(argument.c_str() + 17, nullptr, 10));
        }
    }
    if (scene != "sponza" && scene != "damaged-helmet" && scene != "stress" && scene != "lucy")
    {
        std::fprintf(stderr,
            "Unsupported scene '%s'. Expected 'damaged-helmet', 'sponza', 'stress', or 'lucy'.\n",
            scene.c_str());
        std::fprintf(stderr, "Press Enter to exit...\n");
        (void)std::getchar();
        return EXIT_FAILURE;
    }

#ifndef HALCYON_ASSET_ROOT
#define HALCYON_ASSET_ROOT "assets"
#endif
    const std::filesystem::path root = HALCYON_ASSET_ROOT;
    const std::filesystem::path helmet = "models/damaged_helmet/DamagedHelmet.glb";
    const std::filesystem::path sponza = "models/sponza/Sponza.gltf";
    const std::filesystem::path lucy = "models/lucy/lucy.ply";
    const std::filesystem::path selected = scene == "sponza" ? sponza :
        scene == "lucy" ? lucy : helmet;
    if (scene != "stress" && !std::filesystem::exists(root / selected))
    {
        std::fprintf(stderr,
            "Scene asset '%s' is missing. Run: cmake --build <build-dir> --target %s\n",
            (root / selected).string().c_str(),
            scene == "lucy" ? "HalcyonFetchM5Assets" : "HalcyonFetchAssets");
        std::fprintf(stderr, "Press Enter to exit...\n");
        (void)std::getchar();
        return EXIT_FAILURE;
    }

    Halcyon::Examples::ExampleDefinition definition{};
    definition.title = "Halcyon Example 03 - PBR Scenes";
    definition.sceneName = scene;
    definition.scene.name = scene;
    definition.enableDiagnostics = true;
    if (scene == "stress")
    {
        Halcyon::ProceduralStressSceneConfig stressConfig;
        stressConfig.instanceCount = instanceCount;
        definition.scene.assets.push_back({scene, Halcyon::makeProceduralStressScene(stressConfig)});
        definition.enableGpuDrivenScene = true;
    }
    else
    {
        definition.scene.assets.push_back({scene, selected});
    }
    definition.scene.instances.push_back({"main", scene});
    definition.animatedInstanceName = "main";

    auto state = std::make_shared<std::string>(scene);
    definition.onInitialize = [state](Halcyon::Engine& engine) -> Halcyon::Result<void>
    {
        Halcyon::Perspective perspective{};
        perspective.verticalFovRadians = glm::radians(*state == "sponza" ? 52.0f : 55.0f);
        perspective.nearPlane = 0.05f;
        perspective.farPlane = *state == "sponza" ? 300.0f : *state == "lucy" ? 1000.0f : 100.0f;
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
        result = *state == "sponza"
                     ? engine.defaultView().lookAt({-9.5f, 1.8f, 0.0f}, {6.0f, 2.4f, 0.0f})
                     : *state == "lucy"
                         ? engine.defaultView().lookAt({0.0f, 0.0f, 4.0f}, {0.0f, 0.0f, 0.0f})
                         : engine.defaultView().lookAt({0.0f, 0.1f, 2.6f}, {0.0f, 0.0f, 0.0f});
        if (!result)
        {
            return result;
        }
        const Halcyon::SceneInstanceHandle instance = engine.sceneManager().findInstance("main");
        if (!engine.sceneManager().rootEntity(instance).isValid())
        {
            return Halcyon::Result<void>::failure(Halcyon::MakeError(
                Halcyon::ErrorCode::NotFound, "configured scene instance is unavailable", "Example"));
        }
        const bool sponza = *state == "sponza";
        const Halcyon::Entity sun = engine.scene().createEntity();
        Halcyon::LightComponent sunLight{};
        sunLight.type = Halcyon::LightType::Directional;
        sunLight.direction = {0.35f, -0.85f, -0.35f};
        sunLight.color = {1.0f, 0.96f, 0.90f};
        sunLight.intensity = sponza ? 1.4f : 3.5f;
        sunLight.range = 1000.0f;
        (void)engine.scene().lights().add(sun, sunLight);
        if (sponza)
        {
            // Approximate bounced skylight along the atrium. A single short-range
            // fill cannot light the ~30 m courtyard from the current camera.
            const glm::vec3 warm{1.0f, 0.94f, 0.86f};
            const std::array<glm::vec3, 3> fillPositions = {
                glm::vec3{-8.0f, 4.5f, 0.0f},
                glm::vec3{0.0f, 6.0f, 0.0f},
                glm::vec3{7.0f, 4.5f, 0.0f}};
            for (const glm::vec3& position : fillPositions)
            {
                const Halcyon::Entity fill = engine.scene().createEntity();
                Halcyon::LightComponent fillLight{};
                fillLight.type = Halcyon::LightType::Point;
                fillLight.position = position;
                fillLight.color = warm;
                fillLight.intensity = 3.5f;
                fillLight.range = 16.0f;
                (void)engine.scene().lights().add(fill, fillLight);
            }
        }
        else
        {
            const Halcyon::Entity fill = engine.scene().createEntity();
            Halcyon::LightComponent fillLight{};
            fillLight.type = Halcyon::LightType::Point;
            fillLight.position = {-1.6f, 1.2f, 2.2f};
            fillLight.color = {1.0f, 0.97f, 0.93f};
            fillLight.intensity = 1.8f;
            fillLight.range = 6.0f;
            (void)engine.scene().lights().add(fill, fillLight);
        }
        return Halcyon::Result<void>::success();
    };
    definition.onFrame = [state](Halcyon::Engine& engine, const Halcyon::FrameInfo& frame)
        -> Halcyon::Result<void>
    {
        if (*state == "sponza")
        {
            return Halcyon::Result<void>::success();
        }
        if (*state == "lucy")
        {
            const Halcyon::SceneInstanceHandle instance = engine.sceneManager().findInstance("main");
            const Halcyon::Entity model = engine.sceneManager().rootEntity(instance);
            auto* transform = engine.scene().transforms().get(model);
            if (transform == nullptr)
                return Halcyon::Result<void>::failure(Halcyon::MakeError(
                    Halcyon::ErrorCode::InvalidState, "Lucy transform is unavailable", "Example"));
            const glm::vec3 center{690.7556f, -121.5314f, 192.6266f};
            // The Stanford Lucy PLY is Z-up and faces +Y. Convert it to the
            // engine's Y-up basis, then face the fixed +Z acceptance camera.
            transform->localTransform = glm::rotate(glm::mat4{1.0f},
                glm::pi<float>(), glm::vec3{0.0f, 1.0f, 0.0f}) *
                glm::rotate(glm::mat4{1.0f}, -glm::half_pi<float>(),
                    glm::vec3{1.0f, 0.0f, 0.0f}) *
                glm::scale(glm::mat4{1.0f}, glm::vec3{0.0015f}) *
                glm::translate(glm::mat4{1.0f}, -center);
            transform->dirty = true;
            return Halcyon::Result<void>::success();
        }
        if (*state == "stress")
        {
            const float angle = static_cast<float>(frame.elapsedSeconds) * glm::radians(9.0f);
            const glm::vec3 position{std::sin(angle) * 20.0f, 1.5f, std::cos(angle) * 20.0f};
            return engine.defaultView().lookAt(position, {0.0f, 0.0f, 0.0f});
        }
        const Halcyon::SceneInstanceHandle instance = engine.sceneManager().findInstance("main");
        const Halcyon::Entity model = engine.sceneManager().rootEntity(instance);
        auto* transform = engine.scene().transforms().get(model);
        if (transform == nullptr)
        {
            return Halcyon::Result<void>::failure(Halcyon::MakeError(
                Halcyon::ErrorCode::InvalidState, "model transform is unavailable", "Example"));
        }
        const float angle = static_cast<float>(frame.elapsedSeconds) * glm::radians(4.0f);
        transform->localTransform = glm::rotate(glm::mat4{1.0f}, angle, {0.0f, 1.0f, 0.0f});
        transform->dirty = true;
        return Halcyon::Result<void>::success();
    };

    return Halcyon::Examples::run(definition, argc, argv);
}
