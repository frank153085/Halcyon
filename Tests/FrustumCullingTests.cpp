#include "Renderer/Scene/GpuScene.h"

#include <array>
#include <iostream>

static bool sphereVisible(const std::array<glm::vec4, 6>& planes, const glm::vec4& sphere)
{
    for (const auto& plane : planes)
        if (glm::dot(glm::vec3(plane), glm::vec3(sphere)) + plane.w + sphere.w < 0.0f)
            return false;
    return true;
}

int main()
{
    const std::array<glm::vec4, 6> planes = {{{1, 0, 0, 1}, {-1, 0, 0, 1},
        {0, 1, 0, 1}, {0, -1, 0, 1}, {0, 0, 1, 1}, {0, 0, -1, 1}}};
    if (!sphereVisible(planes, {0, 0, 0, 0.1f}) || sphereVisible(planes, {3, 0, 0, 0.1f}))
        return 1;
    std::cout << "Frustum culling tests passed\n";
    return 0;
}
