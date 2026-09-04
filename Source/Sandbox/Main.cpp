#include "Halcyon/Application.h"
#include "SandboxExample.h"

#include <filesystem>
#include <utility>

#ifndef HALCYON_ASSET_ROOT
#define HALCYON_ASSET_ROOT "assets"
#endif

int main(int argc, char** argv)
{
    Halcyon::ApplicationConfig config;
    config.window.title = "Halcyon Sandbox";
    config.engine.scene.name = "sandbox";
    config.engine.scene.assetRoot = HALCYON_ASSET_ROOT;
    config.engine.scene.assets.push_back(
        {"monkey", std::filesystem::path{"models/monkey/monkey.gltf"}});
    config.engine.scene.instances.push_back({"main", "monkey"});
    config.enableDiagnostics = true;
    return Halcyon::Application::run(
        argc, argv, std::move(config), Halcyon::Sandbox::makeCallbacks());
}
