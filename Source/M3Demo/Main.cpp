#include "M3Demo.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#ifndef HALCYON_ASSET_ROOT
#define HALCYON_ASSET_ROOT "assets"
#endif

int main(int argc, char** argv)
{
    std::string scene = "damaged-helmet";
    std::size_t instanceCount = 100'000;
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i] != nullptr ? argv[i] : "";
        if (argument == "--help" || argument == "-h")
        {
            // Let the common runner print the complete option list without
            // requiring downloaded M3 assets or a Vulkan device.
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
        return EXIT_FAILURE;
    }
    Halcyon::ApplicationConfig config;
    config.window.title = "Halcyon M3 - " + scene;
    config.sceneName = scene;
    const std::filesystem::path root = HALCYON_ASSET_ROOT;
    const std::filesystem::path helmet = "m3/DamagedHelmet.glb";
    const std::filesystem::path sponza = "m3/Sponza/Sponza.gltf";
    const std::filesystem::path selected = scene == "sponza" ? sponza : helmet;
    if (scene != "stress" && !std::filesystem::exists(root / selected))
    {
        std::fprintf(stderr,
            "M3 asset '%s' is missing. Run: cmake --build out/build/m3-msvc-debug "
            "--target HalcyonFetchM3Assets\n",
            (root / selected).string().c_str());
        return EXIT_FAILURE;
    }
    config.engine.scene.name = scene;
    config.engine.scene.assetRoot = root;
    if (scene == "stress")
    {
        Halcyon::ProceduralStressSceneConfig stressConfig;
        stressConfig.instanceCount = instanceCount;
        config.engine.scene.assets.push_back(
            {scene, Halcyon::makeProceduralStressScene(stressConfig)});
    }
    else
    {
        config.engine.scene.assets.push_back({scene, selected});
    }
    config.engine.scene.instances.push_back({"main", scene});
    config.enableDiagnostics = true;
    return Halcyon::Application::run(
        argc, argv, std::move(config), Halcyon::M3Demo::makeCallbacks(scene));
}
