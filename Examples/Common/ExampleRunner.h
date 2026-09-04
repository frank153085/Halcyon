#pragma once

#include "Halcyon/Application.h"

#include <functional>

namespace Halcyon::Examples
{

struct ExampleDefinition
{
    const char* title = "Halcyon Example";
    SceneManagerConfig scene;
    std::string animatedInstanceName;
    std::function<Result<void>(Engine&)> onInitialize;
    std::function<Result<void>(Engine&, const FrameInfo&)> onFrame;
    std::function<void(Engine&)> onShutdown;
};

[[nodiscard]] StaticScene makeTriangleScene();
int run(const ExampleDefinition& definition, int argc, char** argv);

} // namespace Halcyon::Examples
