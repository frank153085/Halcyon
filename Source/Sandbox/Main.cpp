#include "Halcyon/Application.h"
#include "SandboxExample.h"

#include <utility>

int main(int argc, char** argv)
{
    Halcyon::ApplicationConfig config;
    config.window.title = "Halcyon Sandbox";
    config.engine.startupTexturePath = "assets/models/monkey/color.png";
    config.engine.startupMeshPath = "assets/models/monkey/monkey.obj";
    config.enableDiagnostics = true;
    return Halcyon::Application::run(
        argc, argv, std::move(config), Halcyon::Sandbox::makeCallbacks());
}
