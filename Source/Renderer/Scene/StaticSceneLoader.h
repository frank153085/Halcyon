#pragma once

#include "../../Core/Result.h"
#include "../Quality/Pbr.h"

#include <cstdint>
#include <filesystem>
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace Halcyon::Renderer::Scene
{

struct StaticSceneVertex
{
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    glm::vec2 uv{0.0f};
    glm::vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
};

struct StaticScenePrimitive
{
    std::vector<StaticSceneVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::uint32_t materialIndex = 0;
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
    // World transform of the node instance that owns this primitive.  glTF
    // stores mesh data in mesh-local space and applies transforms on nodes;
    // keeping the transform with the decoded primitive lets uploaders build a
    // compact static mesh while preserving the scene hierarchy.
    glm::mat4 worldTransform{1.0f};
};

struct StaticSceneMaterial
{
    std::string name;
    Quality::PbrMaterial pbr{};
    std::string baseColorTexture;
    std::string normalTexture;
    std::string metallicRoughnessTexture;
    std::string emissiveTexture;
    bool doubleSided = false;
    bool transparent = false;
    bool alphaMasked = false;
    float alphaCutoff = 0.5f;
    std::string occlusionTexture;
};

struct StaticSceneNode
{
    std::string name;
    glm::mat4 localTransform{1.0f};
    glm::mat4 worldTransform{1.0f};
    std::int32_t parent = -1;
    std::vector<std::uint32_t> primitiveIndices;
};

struct StaticScene
{
    std::string sourcePath;
    std::vector<StaticScenePrimitive> primitives;
    std::vector<StaticSceneMaterial> materials;
    std::vector<StaticSceneNode> nodes;

    [[nodiscard]] bool empty() const noexcept
    {
        return primitives.empty();
    }
};

struct StaticSceneLoadOptions
{
    bool generateMissingNormals = true;
    bool generateMissingTangents = true;
    bool flipV = true;
    bool rejectUnsupportedPrimitives = true;
};

// Loads a static glTF/GLB scene through the vendored reader.  The
// resulting data is backend-neutral and suitable for either a Vulkan upload or
// deterministic CPU image tests.  Skeletal animation and morph targets are
// intentionally rejected because M3 covers rigid scenes only.
[[nodiscard]] Halcyon::Result<StaticScene> loadStaticScene(
    const std::filesystem::path& path, const StaticSceneLoadOptions& options = {});

// Naming aliases keep call sites readable when the asset pipeline is switched
// between cgltf and fastgltf implementations in later milestones.
[[nodiscard]] inline Halcyon::Result<StaticScene> loadGltfScene(
    const std::filesystem::path& path, const StaticSceneLoadOptions& options = {})
{
    return loadStaticScene(path, options);
}

class FastGltfSceneLoader final
{
public:
    [[nodiscard]] Halcyon::Result<StaticScene> load(
        const std::filesystem::path& path, const StaticSceneLoadOptions& options = {}) const
    {
        return loadStaticScene(path, options);
    }
};

} // namespace Halcyon::Renderer::Scene
