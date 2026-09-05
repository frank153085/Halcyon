#include "Halcyon/Scene.h"

#include <chrono>
#include <iostream>

int main()
{
    Halcyon::ProceduralStressSceneConfig config;
    config.instanceCount = 100'000;
    const auto begin = std::chrono::steady_clock::now();
    const Halcyon::StaticScene scene = Halcyon::makeProceduralStressScene(config);
    const double milliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - begin).count();
    if (scene.primitives.size() != 1 || scene.nodes.size() != config.instanceCount)
    {
        std::cerr << "stress scene geometry/instance contract failed\n";
        return 1;
    }
    for (const auto& node : scene.nodes)
    {
        if (node.primitiveIndices.size() != 1 || node.primitiveIndices.front() != 0u)
        {
            std::cerr << "stress scene mesh sharing contract failed\n";
            return 2;
        }
    }
    std::cout << "Stress scene generated " << scene.nodes.size()
              << " instances in " << milliseconds << " ms\n";
    return 0;
}
