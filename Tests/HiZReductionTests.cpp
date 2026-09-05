#include "Renderer/Scene/GpuScene.h"

#include <algorithm>
#include <array>
#include <iostream>

int main()
{
    const std::vector<float> patch = {0.8f, 0.2f, 0.6f, 0.4f, 0.9f, 0.7f};
    std::uint32_t width = 0, height = 0;
    const auto reduced = Halcyon::Renderer::Scene::reduceHiZ2x2Min(patch, 3, 2, width, height);
    if (width != 2 || height != 1 || reduced.size() != 2 || reduced[0] != 0.2f || reduced[1] != 0.6f)
        return 1;
    // Reversed-Z conservative reduction must retain the minimum value.
    std::cout << "Hi-Z reduction tests passed\n";
    return 0;
}
