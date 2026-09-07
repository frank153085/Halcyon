#include "ExampleRunner.h"

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
    if (scene != "sponza" && scene != "damaged-helmet" && scene != "stress")
    {
        std::fprintf(stderr,
            "Unsupported scene '%s'. Expected 'damaged-helmet', 'sponza', or 'stress'.\n",
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
    const std::filesystem::path selected = scene == "sponza" ? sponza : helmet;
    if (scene != "stress" && !std::filesystem::exists(root / selected))
    {
        std::fprintf(stderr,
            "Scene asset '%s' is missing. Run: cmake --build <build-dir> --target HalcyonFetchAssets\n",
            (root / selected).string().c_str());
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
        perspective.farPlane = *state == "sponza" ? 300.0f : 100.0f;
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
                     ? engine.defaultView().lookAt({0.0f, 2.5f, 0.0f}, {0.0f, 2.5f, -1.0f})
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
        sunLight.intensity = sponza ? 0.65f : 3.5f;
        sunLight.range = 1000.0f;
        (void)engine.scene().lights().add(sun, sunLight);
        const Halcyon::Entity fill = engine.scene().createEntity();
        Halcyon::LightComponent fillLight{};
        fillLight.type = Halcyon::LightType::Point;
        fillLight.position = sponza ? glm::vec3{2.0f, 2.5f, 2.0f} : glm::vec3{-1.6f, 1.2f, 2.2f};
        fillLight.color = sponza ? glm::vec3{0.55f, 0.65f, 0.90f} : glm::vec3{1.0f, 0.97f, 0.93f};
        fillLight.intensity = sponza ? 1.5f : 1.8f;
        fillLight.range = sponza ? 8.0f : 6.0f;
        (void)engine.scene().lights().add(fill, fillLight);
        return Halcyon::Result<void>::success();
    };
    definition.onFrame = [state](Halcyon::Engine& engine, const Halcyon::FrameInfo& frame)
        -> Halcyon::Result<void>
    {
        if (*state == "sponza")
        {
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
