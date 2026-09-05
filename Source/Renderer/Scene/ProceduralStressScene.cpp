#include "ProceduralStressScene.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>
#include <utility>
#include <glm/gtc/matrix_transform.hpp>

namespace Halcyon::Renderer::Scene
{

StaticScene makeProceduralStressScene(const ProceduralStressSceneConfig& config)
{
    const std::size_t count = std::max<std::size_t>(1, config.instanceCount);
    StaticScene scene;
    scene.sourcePath = "procedural://stress";
    StaticSceneMaterial material;
    material.name = "stress-default";
    material.pbr.baseColor = glm::vec4(0.7f, 0.75f, 0.8f, 1.0f);
    material.pbr.roughness = 0.7f;
    scene.materials.push_back(material);

    constexpr glm::vec3 positions[] = {
        {-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f},
        {-0.5f, 0.5f, -0.5f}, {-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f},
        {0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}};
    constexpr std::uint32_t indices[] = {0, 1, 2, 2, 3, 0, 1, 5, 6, 6, 2, 1,
        5, 4, 7, 7, 6, 5, 4, 0, 3, 3, 7, 4, 3, 2, 6, 6, 7, 3, 4, 5, 1, 1, 0, 4};
    scene.primitives.reserve(count);
    scene.nodes.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        StaticScenePrimitive primitive;
        primitive.materialIndex = 0;
        primitive.indices.assign(std::begin(indices), std::end(indices));
        primitive.vertices.reserve(8);
        for (std::size_t vertex = 0; vertex < 8; ++vertex)
        {
            primitive.vertices.push_back({positions[vertex], glm::normalize(positions[vertex]),
                {positions[vertex].x + 0.5f, positions[vertex].y + 0.5f}, {1, 0, 0, 1}});
        }
        primitive.boundsMin = {-0.5f, -0.5f, -0.5f};
        primitive.boundsMax = {0.5f, 0.5f, 0.5f};
        const std::size_t columns = static_cast<std::size_t>(std::ceil(std::sqrt(
            static_cast<double>(count))));
        const float x = static_cast<float>(i % columns) * config.gridSpacing;
        const float z = static_cast<float>(i / columns) * config.gridSpacing;
        primitive.worldTransform = glm::translate(glm::mat4(1.0f), {x, 0.0f, z});
        scene.primitives.push_back(std::move(primitive));
        StaticSceneNode node;
        node.name = config.baseMeshName + "-" + std::to_string(i);
        node.localTransform = primitive.worldTransform;
        node.worldTransform = scene.primitives.back().worldTransform;
        node.primitiveIndices.push_back(static_cast<std::uint32_t>(i));
        scene.nodes.push_back(std::move(node));
    }
    return scene;
}

} // namespace Halcyon::Renderer::Scene
