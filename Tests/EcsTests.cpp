#include "Renderer/Resources/ResourceTypes.h"
#include "Renderer/Scene/Ecs/RenderExtractor.h"

#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <string_view>
#include <utility>

namespace
{

class TestContext
{
public:
    void expect(bool condition, std::string_view expression, int line)
    {
        if (condition)
        {
            return;
        }
        ++failures_;
        std::cerr << "FAILED line " << line << ": " << expression << '\n';
    }

    [[nodiscard]] int failures() const noexcept
    {
        return failures_;
    }

private:
    int failures_ = 0;
};

#define HALCYON_EXPECT(context, expression)                                                        \
    (context).expect(static_cast<bool>(expression), #expression, __LINE__)

using Halcyon::Renderer::Scene::Ecs::Entity;
using Halcyon::Renderer::Scene::Ecs::EntityManager;
using Halcyon::Renderer::Scene::Ecs::LightComponent;
using Halcyon::Renderer::Scene::Ecs::RenderableComponent;
using Halcyon::Renderer::Scene::Ecs::RenderExtractor;
using Halcyon::Renderer::Scene::Ecs::Scene;
using Halcyon::Renderer::Scene::Ecs::TransformComponent;

void entityTests(TestContext& context)
{
    EntityManager manager;
    const Entity first = manager.create();
    HALCYON_EXPECT(context, first.isValid());
    HALCYON_EXPECT(context, first.index() != 0);
    HALCYON_EXPECT(context, manager.isAlive(first));
    manager.destroy(first);
    HALCYON_EXPECT(context, !manager.isAlive(first));

    const Entity replacement = manager.create();
    HALCYON_EXPECT(context, replacement.index() == first.index());
    HALCYON_EXPECT(context, replacement.generation() != first.generation());
    HALCYON_EXPECT(context, !manager.isAlive(first));
    HALCYON_EXPECT(context, manager.isAlive(replacement));
    HALCYON_EXPECT(context, manager.aliveCount() == 1);

    manager.clear();
    HALCYON_EXPECT(context, manager.aliveCount() == 0);
    HALCYON_EXPECT(context, !manager.isAlive(replacement));
    const Entity afterClear = manager.create();
    HALCYON_EXPECT(context, manager.isAlive(afterClear));
    HALCYON_EXPECT(context, !manager.isAlive(replacement));
}

void deltaExtractionTests(TestContext& context)
{
    Scene scene;
    const Entity entity = scene.createEntity();
    (void)scene.transforms().add(entity);
    (void)scene.renderables().add(entity);
    scene.updateTransforms();
    RenderExtractor extractor;
    const auto first = extractor.extractDelta(scene, {}, 1u);
    HALCYON_EXPECT(context, first.created.size() == 1u && first.updated.empty());
    const auto second = extractor.extractDelta(scene, {}, 2u);
    HALCYON_EXPECT(context, second.empty());
    scene.transforms().get(entity)->localTransform[3].x = 2.0f;
    scene.updateTransforms();
    const auto third = extractor.extractDelta(scene, {}, 3u);
    HALCYON_EXPECT(context, third.updated.size() == 1u);

    auto* renderable = scene.renderables().get(entity);
    HALCYON_EXPECT(context, renderable != nullptr);
    renderable->flags = 17u;
    const auto materialChange = extractor.extractDelta(scene, {}, 4u);
    HALCYON_EXPECT(context, materialChange.updated.size() == 1u);
    HALCYON_EXPECT(context, materialChange.updated[0].instance.flags == 17u);

    scene.destroyEntity(entity);
    const auto fourth = extractor.extractDelta(scene, {}, 5u);
    HALCYON_EXPECT(context, fourth.destroyed.size() == 1u);
}

void componentTests(TestContext& context)
{
    Scene scene;
    const Entity first = scene.createEntity();
    const Entity second = scene.createEntity();
    (void)scene.transforms().add(first);
    (void)scene.transforms().add(second);
    (void)scene.renderables().add(first);
    (void)scene.renderables().add(second);
    HALCYON_EXPECT(context, scene.contains(first));
    HALCYON_EXPECT(context, scene.renderables().size() == 2);

    (void)scene.renderables().remove(first);
    HALCYON_EXPECT(context, !scene.renderables().has(first));
    HALCYON_EXPECT(context, scene.renderables().has(second));
    HALCYON_EXPECT(context, scene.renderables().size() == 1);

    scene.destroyEntity(second);
    HALCYON_EXPECT(context, !scene.contains(second));
    HALCYON_EXPECT(context, !scene.entities().isAlive(second));
    HALCYON_EXPECT(context, !scene.transforms().has(second));
}

void transformAndExtractionTests(TestContext& context)
{
    Scene scene;
    const Entity parent = scene.createEntity();
    const Entity child = scene.createEntity();
    TransformComponent parentTransform;
    parentTransform.localTransform = glm::translate(glm::mat4{1.0f}, glm::vec3{2.0f, 0.0f, 0.0f});
    (void)scene.transforms().add(parent, parentTransform);
    TransformComponent childTransform;
    childTransform.parent = parent;
    childTransform.localTransform = glm::translate(glm::mat4{1.0f}, glm::vec3{0.0f, 3.0f, 0.0f});
    (void)scene.transforms().add(child, childTransform);

    RenderableComponent renderable;
    renderable.mesh = Halcyon::Renderer::Resources::MeshHandle::fromParts(7u, 1u);
    renderable.material = Halcyon::Renderer::Resources::MaterialHandle::fromParts(9u, 1u);
    renderable.flags = 3u;
    (void)scene.renderables().add(child, renderable);

    LightComponent light;
    light.position = {1.0f, 2.0f, 3.0f};
    light.range = 4.0f;
    light.intensity = 5.0f;
    (void)scene.lights().add(parent, light);

    scene.updateTransforms();
    const auto* world = scene.transforms().get(child);
    HALCYON_EXPECT(context, world != nullptr);
    HALCYON_EXPECT(context, std::abs(world->worldTransform[3].x - 2.0f) < 0.001f);
    HALCYON_EXPECT(context, std::abs(world->worldTransform[3].y - 3.0f) < 0.001f);

    const auto packet = RenderExtractor::extract(scene, {}, 42u);
    HALCYON_EXPECT(context, packet.frameIndex == 42u);
    HALCYON_EXPECT(context, packet.instances.size() == 1u);
    HALCYON_EXPECT(context, packet.lights.size() == 1u);
    HALCYON_EXPECT(context, packet.instances[0].meshId == 7u);
    HALCYON_EXPECT(context, packet.instances[0].materialId == 9u);
    HALCYON_EXPECT(context, packet.instances[0].flags == 3u);
    HALCYON_EXPECT(context, packet.lights[0].positionAndRadius[3] == 4.0f);

    // A parent-only edit must propagate to descendants, while a subsequent
    // static update must report no work.
    auto* mutableParent = scene.transforms().get(parent);
    mutableParent->localTransform = glm::translate(
        glm::mat4{1.0f}, glm::vec3{5.0f, 0.0f, 0.0f});
    scene.updateTransforms();
    const auto updated = scene.transforms().updatedEntities();
    HALCYON_EXPECT(context, updated.size() == 2u);
    const auto* movedChild = std::as_const(scene).transforms().get(child);
    HALCYON_EXPECT(context, std::abs(movedChild->worldTransform[3].x - 5.0f) < 0.001f);
    scene.updateTransforms();
    HALCYON_EXPECT(context, scene.transforms().updatedEntities().empty());

    // Malformed parent cycles are contained and never duplicate updates.
    mutableParent = scene.transforms().get(parent);
    mutableParent->parent = child;
    scene.transforms().get(child)->parent = parent;
    scene.updateTransforms();
    HALCYON_EXPECT(context, scene.transforms().updatedEntities().size() == 2u);
}

} // namespace

int main()
{
    TestContext context;
    entityTests(context);
    deltaExtractionTests(context);
    componentTests(context);
    transformAndExtractionTests(context);

    if (context.failures() != 0)
    {
        std::cerr << context.failures() << " ECS test(s) failed\n";
        return 1;
    }
    std::cout << "All ECS tests passed\n";
    return 0;
}
