#include "Halcyon/SceneManager.h"
#include "Renderer/Scene/Ecs/RenderExtractor.h"

#include <cmath>
#include <filesystem>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <string_view>

#ifndef HALCYON_SOURCE_DIR
#define HALCYON_SOURCE_DIR "."
#endif

namespace
{

class TestContext
{
public:
    void expect(bool condition, std::string_view expression, int line)
    {
        if (!condition)
        {
            ++failures_;
            std::cerr << "FAILED line " << line << ": " << expression << '\n';
        }
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

[[nodiscard]] Halcyon::StaticScene makeTriangleScene()
{
    Halcyon::StaticScene scene;
    scene.sourcePath = "memory://scene-manager-test";
    Halcyon::StaticSceneMaterial material;
    material.pbr.baseColor = {0.5f, 0.25f, 0.0f, 1.0f};
    scene.materials.push_back(material);
    Halcyon::StaticScenePrimitive primitive;
    primitive.vertices = {
        Halcyon::StaticSceneVertex{{-1.0f, -1.0f, 0.0f}},
        Halcyon::StaticSceneVertex{{1.0f, -1.0f, 0.0f}},
        Halcyon::StaticSceneVertex{{0.0f, 1.0f, 0.0f}},
    };
    primitive.indices = {0, 1, 2};
    primitive.boundsMin = {-1.0f, -1.0f, 0.0f};
    primitive.boundsMax = {1.0f, 1.0f, 0.0f};
    scene.primitives.push_back(std::move(primitive));

    Halcyon::StaticSceneNode parent;
    parent.name = "Parent";
    parent.localTransform = glm::translate(glm::mat4{1.0f}, {0.0f, 2.0f, 0.0f});
    scene.nodes.push_back(parent);
    Halcyon::StaticSceneNode child;
    child.name = "Child";
    child.parent = 0;
    child.localTransform = glm::translate(glm::mat4{1.0f}, {0.0f, 0.0f, 3.0f});
    child.primitiveIndices.push_back(0);
    scene.nodes.push_back(child);
    return scene;
}

void proceduralLifecycleTests(TestContext& context)
{
    Halcyon::SceneManager manager;
    auto malformed = makeTriangleScene();
    malformed.primitives.front().indices.push_back(99u);
    const auto malformedResult = manager.createAsset("malformed", std::move(malformed));
    HALCYON_EXPECT(context, !malformedResult);
    HALCYON_EXPECT(context, malformedResult.error().code == Halcyon::ErrorCode::InvalidArgument);
    HALCYON_EXPECT(context, manager.database().meshCount() == 0);

    const auto asset = manager.createAsset("triangle", makeTriangleScene());
    HALCYON_EXPECT(context, asset);
    HALCYON_EXPECT(context, manager.findAsset("triangle") == asset.value());
    HALCYON_EXPECT(context, manager.database().meshCount() == 1);
    HALCYON_EXPECT(context, manager.database().materialCount() == 1);
    HALCYON_EXPECT(context, manager.database().textureCount() == 5);

    // A second asset with identical missing textures reuses the manager's
    // deterministic defaults instead of creating duplicate GPU records.
    const auto duplicateDefaults = manager.createAsset("triangle-copy", makeTriangleScene());
    HALCYON_EXPECT(context, duplicateDefaults);
    HALCYON_EXPECT(context, manager.database().textureCount() == 5);

    const auto duplicate = manager.createAsset("triangle", makeTriangleScene());
    HALCYON_EXPECT(context, !duplicate);
    HALCYON_EXPECT(context, duplicate.error().code == Halcyon::ErrorCode::AlreadyExists);
    const auto missingAsset = manager.createInstance({"bad", "missing"});
    HALCYON_EXPECT(context, !missingAsset);
    HALCYON_EXPECT(context, missingAsset.error().code == Halcyon::ErrorCode::NotFound);

    const glm::mat4 firstTransform = glm::translate(glm::mat4{1.0f}, {4.0f, 0.0f, 0.0f});
    const auto first = manager.createInstance({"first", "triangle", firstTransform});
    const auto second = manager.createInstance({"second", "triangle"});
    HALCYON_EXPECT(context, first);
    HALCYON_EXPECT(context, second);
    const auto duplicateInstance = manager.createInstance({"second", "triangle"});
    HALCYON_EXPECT(context, !duplicateInstance);
    HALCYON_EXPECT(context, duplicateInstance.error().code == Halcyon::ErrorCode::AlreadyExists);
    HALCYON_EXPECT(context, manager.rootEntity(first.value()).isValid());
    HALCYON_EXPECT(
        context, manager.rootEntity(first.value()) != manager.rootEntity(second.value()));
    HALCYON_EXPECT(context, manager.scene().renderables().size() == 2);

    manager.scene().updateTransforms();
    const auto packet =
        Halcyon::Renderer::Scene::Ecs::RenderExtractor::extract(manager.scene(), {});
    HALCYON_EXPECT(context, packet.instances.size() == 2);
    if (!packet.instances.empty())
    {
        const auto renderableEntity = manager.scene().renderables().entities().front();
        const auto* renderable = manager.scene().renderables().get(renderableEntity);
        HALCYON_EXPECT(context, renderable != nullptr);
        HALCYON_EXPECT(context, packet.instances.front().meshId == renderable->mesh.index());
        HALCYON_EXPECT(
            context, packet.instances.front().materialId == renderable->material.index());
        const auto* material = manager.database().get(renderable->material);
        HALCYON_EXPECT(context, material != nullptr);
        if (material != nullptr)
        {
            const auto* texture = manager.database().get(material->baseColorTexture);
            HALCYON_EXPECT(context, texture != nullptr);
            if (texture != nullptr)
            {
                HALCYON_EXPECT(context, texture->generatedDefault);
                HALCYON_EXPECT(context, texture->solidColor[0] == 128);
                HALCYON_EXPECT(context, texture->solidColor[1] == 64);
                HALCYON_EXPECT(context, texture->solidColor[2] == 0);
            }
        }
    }
    bool foundTranslatedPrimitive = false;
    for (const Halcyon::Entity entity : manager.scene().renderables().entities())
    {
        const auto* transform = manager.scene().transforms().get(entity);
        if (transform != nullptr && std::abs(transform->worldTransform[3].x - 4.0f) < 0.001f &&
            std::abs(transform->worldTransform[3].y - 2.0f) < 0.001f &&
            std::abs(transform->worldTransform[3].z - 3.0f) < 0.001f)
        {
            foundTranslatedPrimitive = true;
        }
    }
    HALCYON_EXPECT(context, foundTranslatedPrimitive);

    const auto liveUnload = manager.unloadAsset(asset.value());
    HALCYON_EXPECT(context, !liveUnload);
    HALCYON_EXPECT(context, liveUnload.error().code == Halcyon::ErrorCode::InvalidState);
    HALCYON_EXPECT(context, manager.destroyInstance(first.value()));
    HALCYON_EXPECT(context, manager.destroyInstance(second.value()));
    HALCYON_EXPECT(context, manager.unloadAsset(asset.value()));
    // The duplicate asset retains the shared deterministic textures; unloading
    // the first asset must not invalidate those records.
    HALCYON_EXPECT(context, manager.database().textureCount() == 5);
    HALCYON_EXPECT(context, manager.unloadAsset(duplicateDefaults.value()));
    HALCYON_EXPECT(context, manager.database().meshCount() == 0);
    HALCYON_EXPECT(context, manager.database().materialCount() == 0);
    HALCYON_EXPECT(context, manager.database().textureCount() == 0);
    HALCYON_EXPECT(context, manager.scene().members().empty());
}

void fileAndShutdownTests(TestContext& context)
{
    Halcyon::SceneManager manager;
    const std::filesystem::path sourceRoot = HALCYON_SOURCE_DIR;
    const auto missing = manager.loadAsset("missing", sourceRoot / "assets/does-not-exist.glb");
    HALCYON_EXPECT(context, !missing);
    HALCYON_EXPECT(context, missing.error().code == Halcyon::ErrorCode::NotFound);
    HALCYON_EXPECT(
        context, missing.error().message.find("does-not-exist.glb") != std::string::npos);

    const auto monkey =
        manager.loadAsset("monkey", sourceRoot / "assets/models/monkey/monkey.gltf");
    HALCYON_EXPECT(context, monkey);
    const auto instance = manager.createInstance({"monkey-instance", "monkey"});
    HALCYON_EXPECT(context, instance);
    HALCYON_EXPECT(context, manager.scene().renderables().size() == 1);
    manager.shutdown();
    HALCYON_EXPECT(context, manager.scene().members().empty());
    HALCYON_EXPECT(context, manager.database().meshCount() == 0);
    HALCYON_EXPECT(context, !manager.findAsset("monkey").isValid());
    HALCYON_EXPECT(context, !manager.findInstance("monkey-instance").isValid());
}

} // namespace

int main()
{
    TestContext context;
    proceduralLifecycleTests(context);
    fileAndShutdownTests(context);
    if (context.failures() != 0)
    {
        std::cerr << context.failures() << " SceneManager test(s) failed\n";
        return 1;
    }
    std::cout << "All SceneManager tests passed\n";
    return 0;
}
