#include "M3Demo.h"

#include <filesystem>
#include <cstdio>
#include <string>

#ifndef HALCYON_SOURCE_DIR
#define HALCYON_SOURCE_DIR "."
#endif

int main(int argc, char** argv)
{
    std::string scene = "damaged-helmet";
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i] != nullptr ? argv[i] : "";
        if (argument == "--scene" && i + 1 < argc) scene = argv[++i];
        else if (argument.rfind("--scene=", 0) == 0) scene = argument.substr(8);
    }
    if (scene != "sponza") scene = "damaged-helmet";
    Halcyon::ApplicationConfig config;
    config.window.title = "Halcyon M3 - " + scene;
    config.sceneName = scene;
    const std::filesystem::path root = HALCYON_SOURCE_DIR;
    const std::filesystem::path helmet = root / "assets" / "m3" / "DamagedHelmet.glb";
    const std::filesystem::path sponza = root / "assets" / "m3" / "Sponza" / "Sponza.gltf";
    const std::filesystem::path selected = scene == "sponza" ? sponza : helmet;
    if (std::filesystem::exists(selected))
    {
        config.engine.startupScenePath = selected;
    }
    else
    {
        std::fprintf(stderr,
            "M3 asset '%s' is missing. Run: cmake --build out/build/m3-msvc-debug "
            "--target HalcyonFetchM3Assets\n",
            selected.string().c_str());
        config.engine.startupTexturePath = (root / "assets/models/monkey/color.png").string();
        config.engine.startupMeshPath = (root / "assets/models/monkey/monkey.obj").string();
    }
    config.enableDiagnostics = true;
    return Halcyon::Application::run(argc, argv, std::move(config),
        Halcyon::M3Demo::makeCallbacks(scene));
}
