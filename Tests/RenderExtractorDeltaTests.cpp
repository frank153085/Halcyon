#include "Renderer/Scene/Ecs/RenderExtractor.h"

#include <iostream>

int main()
{
    using namespace Halcyon::Renderer::Scene::Ecs;
    Scene scene;
    const Entity entity = scene.createEntity();
    (void)scene.transforms().add(entity);
    (void)scene.renderables().add(entity);
    scene.updateTransforms();
    RenderExtractor extractor;
    if (extractor.extractDelta(scene, {}, 0).created.size() != 1)
        return 1;
    scene.transforms().get(entity)->localTransform[3].x = 1.0f;
    scene.updateTransforms();
    if (extractor.extractDelta(scene, {}, 1).updated.size() != 1)
        return 2;
    scene.destroyEntity(entity);
    if (extractor.extractDelta(scene, {}, 2).destroyed.size() != 1)
        return 3;
    std::cout << "Render extractor delta tests passed\n";
    return 0;
}
