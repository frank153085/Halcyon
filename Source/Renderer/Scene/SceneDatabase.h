#pragma once

#include "../../Core/HandlePool.h"
#include "../Quality/Pbr.h"
#include "../Resources/ResourceTypes.h"
#include "StaticSceneLoader.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Halcyon::Renderer::Scene
{

struct SceneMesh
{
    std::string name;
    std::vector<std::byte> vertexData;
    std::vector<std::uint32_t> indices;
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
};

struct SceneMaterial
{
    std::string name;
    Quality::PbrMaterial pbr{};
    Resources::TextureHandle baseColorTexture{};
    Resources::TextureHandle normalTexture{};
    Resources::TextureHandle metallicRoughnessTexture{};
    Resources::TextureHandle emissiveTexture{};
    bool transparent = false;
    bool doubleSided = false;
    bool alphaMasked = false;
    float alphaCutoff = 0.5f;
    Resources::TextureHandle occlusionTexture{};
};

struct SceneTexture
{
    std::string path;
    bool srgb = false;
    bool generatedDefault = false;
};

struct SceneImportResult
{
    std::vector<Resources::MeshHandle> meshes;
    std::vector<Resources::MaterialHandle> materials;
    std::vector<Resources::TextureHandle> textures;
};

// Persistent CPU-side asset registry.  It deliberately stores no Vulkan
// objects; a backend resolves the stable handles to GPU allocations during an
// upload phase and can rebuild those allocations without invalidating scene
// packets.
class SceneDatabase final
{
public:
    using MeshHandle = Resources::MeshHandle;
    using MaterialHandle = Resources::MaterialHandle;
    using TextureHandle = Resources::TextureHandle;

    [[nodiscard]] Halcyon::Result<MeshHandle> createMesh(SceneMesh mesh)
    {
        return meshes_.tryEmplace(std::move(mesh));
    }

    [[nodiscard]] Halcyon::Result<MaterialHandle> createMaterial(SceneMaterial material)
    {
        return materials_.tryEmplace(std::move(material));
    }

    [[nodiscard]] Halcyon::Result<TextureHandle> createTexture(SceneTexture texture)
    {
        return textures_.tryEmplace(std::move(texture));
    }

    [[nodiscard]] Halcyon::Result<SceneImportResult> importStaticScene(const StaticScene& scene)
    {
        SceneImportResult imported;
        textureMap_.clear();
        const auto importTexture = [&](const std::string& path, bool srgb, const char* fallback)
            -> Halcyon::Result<TextureHandle>
        {
            const std::string key = path.empty() ? std::string(fallback) : path;
            const std::string mapKey = (srgb ? "s:" : "l:") + key;
            const auto found = textureMap_.find(mapKey);
            if (found != textureMap_.end())
            {
                return Halcyon::Result<TextureHandle>::success(found->second);
            }
            auto created = createTexture(SceneTexture{key, srgb, path.empty()});
            if (created)
            {
                textureMap_.emplace(mapKey, created.value());
                imported.textures.push_back(created.value());
            }
            return created;
        };
        imported.materials.reserve(scene.materials.size());
        imported.meshes.reserve(scene.primitives.size());
        for (const auto& source : scene.materials)
        {
            SceneMaterial material;
            material.name = source.name;
            material.pbr = source.pbr;
            material.transparent = source.transparent;
            material.doubleSided = source.doubleSided;
            material.alphaMasked = source.alphaMasked;
            material.alphaCutoff = source.alphaCutoff;
            const auto baseColor = importTexture(
                source.baseColorTexture, true, "__halcyon_default_white_srgb__");
            const auto normal = importTexture(
                source.normalTexture, false, "__halcyon_default_normal__");
            const auto metallicRoughness = importTexture(
                source.metallicRoughnessTexture, false, "__halcyon_default_black__");
            const auto occlusion = importTexture(
                source.occlusionTexture, false, "__halcyon_default_white__");
            const auto emissive = importTexture(
                source.emissiveTexture, true, "__halcyon_default_black_srgb__");
            if (!baseColor || !normal || !metallicRoughness || !occlusion || !emissive)
            {
                rollback(imported);
                return Halcyon::Result<SceneImportResult>::failure(
                    Halcyon::Error{Halcyon::ErrorCode::OutOfMemory,
                        "failed to allocate scene material textures"});
            }
            material.baseColorTexture = baseColor.value();
            material.normalTexture = normal.value();
            material.metallicRoughnessTexture = metallicRoughness.value();
            material.occlusionTexture = occlusion.value();
            material.emissiveTexture = emissive.value();
            const auto handle = createMaterial(std::move(material));
            if (!handle)
            {
                rollback(imported);
                return Halcyon::Result<SceneImportResult>::failure(handle.error());
            }
            imported.materials.push_back(handle.value());
        }
        if (imported.materials.empty())
        {
            SceneMaterial defaultMaterial{};
            const auto baseColor = importTexture(
                {}, true, "__halcyon_default_white_srgb__");
            const auto normal = importTexture(
                {}, false, "__halcyon_default_normal__");
            const auto metallicRoughness = importTexture(
                {}, false, "__halcyon_default_black__");
            const auto occlusion = importTexture(
                {}, false, "__halcyon_default_white__");
            const auto emissive = importTexture(
                {}, true, "__halcyon_default_black_srgb__");
            if (!baseColor || !normal || !metallicRoughness || !occlusion || !emissive)
            {
                rollback(imported);
                return Halcyon::Result<SceneImportResult>::failure(
                    Halcyon::Error{Halcyon::ErrorCode::OutOfMemory,
                        "failed to allocate default material textures"});
            }
            defaultMaterial.baseColorTexture = baseColor.value();
            defaultMaterial.normalTexture = normal.value();
            defaultMaterial.metallicRoughnessTexture = metallicRoughness.value();
            defaultMaterial.occlusionTexture = occlusion.value();
            defaultMaterial.emissiveTexture = emissive.value();
            const auto handle = createMaterial(std::move(defaultMaterial));
            if (!handle)
            {
                return Halcyon::Result<SceneImportResult>::failure(handle.error());
            }
            imported.materials.push_back(handle.value());
        }
        for (const auto& source : scene.primitives)
        {
            SceneMesh mesh;
            mesh.vertexData.resize(source.vertices.size() * sizeof(StaticSceneVertex));
            if (!source.vertices.empty())
            {
                const auto bytes =
                    std::as_bytes(std::span<const StaticSceneVertex>{source.vertices});
                std::copy(bytes.begin(), bytes.end(), mesh.vertexData.begin());
            }
            mesh.indices = source.indices;
            mesh.boundsMin = source.boundsMin;
            mesh.boundsMax = source.boundsMax;
            const auto handle = createMesh(std::move(mesh));
            if (!handle)
            {
                rollback(imported);
                return Halcyon::Result<SceneImportResult>::failure(handle.error());
            }
            imported.meshes.push_back(handle.value());
        }
        return Halcyon::Result<SceneImportResult>::success(std::move(imported));
    }

    [[nodiscard]] Halcyon::Result<SceneImportResult> import(const StaticScene& scene)
    {
        return importStaticScene(scene);
    }

    [[nodiscard]] SceneMesh* get(MeshHandle handle) noexcept
    {
        return meshes_.get(handle);
    }
    [[nodiscard]] const SceneMesh* get(MeshHandle handle) const noexcept
    {
        return meshes_.get(handle);
    }
    [[nodiscard]] SceneMaterial* get(MaterialHandle handle) noexcept
    {
        return materials_.get(handle);
    }
    [[nodiscard]] const SceneMaterial* get(MaterialHandle handle) const noexcept
    {
        return materials_.get(handle);
    }
    [[nodiscard]] SceneTexture* get(TextureHandle handle) noexcept
    {
        return textures_.get(handle);
    }
    [[nodiscard]] const SceneTexture* get(TextureHandle handle) const noexcept
    {
        return textures_.get(handle);
    }

    [[nodiscard]] bool destroy(MeshHandle handle) noexcept
    {
        return meshes_.erase(handle);
    }
    [[nodiscard]] bool destroy(MaterialHandle handle) noexcept
    {
        return materials_.erase(handle);
    }
    [[nodiscard]] bool destroy(TextureHandle handle) noexcept
    {
        return textures_.erase(handle);
    }
    void clear() noexcept
    {
        meshes_.clear();
        materials_.clear();
        textures_.clear();
        textureMap_.clear();
    }
    [[nodiscard]] std::size_t meshCount() const noexcept
    {
        return meshes_.size();
    }
    [[nodiscard]] std::size_t materialCount() const noexcept
    {
        return materials_.size();
    }
    [[nodiscard]] std::size_t textureCount() const noexcept
    {
        return textures_.size();
    }

private:
    void rollback(const SceneImportResult& imported) noexcept
    {
        for (const auto handle : imported.meshes)
        {
            (void)meshes_.erase(handle);
        }
        for (const auto handle : imported.materials)
        {
            (void)materials_.erase(handle);
        }
        for (const auto handle : imported.textures)
        {
            (void)textures_.erase(handle);
        }
    }

    Core::HandlePool<SceneMesh, Resources::MeshHandle> meshes_;
    Core::HandlePool<SceneMaterial, Resources::MaterialHandle> materials_;
    Core::HandlePool<SceneTexture, Resources::TextureHandle> textures_;
    std::unordered_map<std::string, TextureHandle> textureMap_;
};

} // namespace Halcyon::Renderer::Scene
