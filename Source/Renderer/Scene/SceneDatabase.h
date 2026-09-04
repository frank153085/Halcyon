#pragma once

#include "../../Core/HandlePool.h"
#include "../Quality/Pbr.h"
#include "../Resources/ResourceTypes.h"
#include "StaticSceneLoader.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
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
    std::array<std::uint8_t, 4> solidColor{255, 255, 255, 255};
};

struct SceneImportResult
{
    std::vector<Resources::MeshHandle> meshes;
    std::vector<Resources::MaterialHandle> materials;
    // Unique texture records retained by this asset import. Texture records
    // are shared across assets and must be released as a batch.
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

    // Drop one asset's texture references, destroying records only after the
    // final asset releases them. This is intentionally separate from destroy,
    // which remains an explicit low-level operation for callers that own a
    // record directly.
    void releaseImportedTextures(std::span<const TextureHandle> handles) noexcept
    {
        for (const TextureHandle handle : handles)
        {
            const auto ref = textureReferences_.find(handle);
            if (ref == textureReferences_.end())
            {
                continue;
            }
            if (ref->second > 1u)
            {
                --ref->second;
                continue;
            }
            textureReferences_.erase(ref);
            (void)destroy(handle);
        }
    }

    [[nodiscard]] Halcyon::Result<SceneImportResult> importStaticScene(const StaticScene& scene)
    {
        // Validate procedural input with the same invariants enforced by the
        // glTF loader. Keeping this at the common import boundary guarantees
        // that in-memory and file-backed assets cannot create malformed GPU
        // records or partially attached ECS instances.
        const std::size_t materialCount = std::max<std::size_t>(1u, scene.materials.size());
        for (const auto& primitive : scene.primitives)
        {
            if (primitive.vertices.empty() || primitive.indices.empty() ||
                primitive.indices.size() % 3u != 0u)
            {
                return Halcyon::Result<SceneImportResult>::failure(Halcyon::Error{
                    Halcyon::ErrorCode::InvalidArgument,
                    "scene primitive has empty or incomplete geometry"});
            }
            if (primitive.materialIndex >= materialCount)
            {
                return Halcyon::Result<SceneImportResult>::failure(Halcyon::Error{
                    Halcyon::ErrorCode::InvalidArgument,
                    "scene primitive material index is out of range"});
            }
            for (const std::uint32_t index : primitive.indices)
            {
                if (index >= primitive.vertices.size())
                {
                    return Halcyon::Result<SceneImportResult>::failure(Halcyon::Error{
                        Halcyon::ErrorCode::InvalidArgument,
                        "scene primitive index is out of range"});
                }
            }
        }
        for (const auto& node : scene.nodes)
        {
            if (node.parent < -1 ||
                (node.parent >= 0 && static_cast<std::size_t>(node.parent) >= scene.nodes.size()))
            {
                return Halcyon::Result<SceneImportResult>::failure(Halcyon::Error{
                    Halcyon::ErrorCode::InvalidArgument,
                    "scene node parent index is out of range"});
            }
            for (const std::uint32_t primitive : node.primitiveIndices)
            {
                if (primitive >= scene.primitives.size())
                {
                    return Halcyon::Result<SceneImportResult>::failure(Halcyon::Error{
                        Halcyon::ErrorCode::InvalidArgument,
                        "scene node primitive index is out of range"});
                }
            }
        }
        SceneImportResult imported;
        const auto importTexture =
            [&](const std::string& path,
                bool srgb,
                const char* fallback,
                std::array<std::uint8_t, 4> solidColor) -> Halcyon::Result<TextureHandle>
        {
            std::string key = path.empty() ? std::string(fallback) : path;
            if (path.empty())
            {
                for (const std::uint8_t channel : solidColor)
                {
                    constexpr char digits[] = "0123456789ABCDEF";
                    key.push_back(digits[channel >> 4u]);
                    key.push_back(digits[channel & 0x0Fu]);
                }
            }
            const std::string mapKey = (srgb ? "s:" : "l:") + key;
            const auto found = textureMap_.find(mapKey);
            if (found != textureMap_.end())
            {
                // Retain one reference per asset, not one per material slot.
                if (std::find(imported.textures.begin(), imported.textures.end(), found->second) ==
                    imported.textures.end())
                {
                    ++textureReferences_[found->second];
                    imported.textures.push_back(found->second);
                }
                return Halcyon::Result<TextureHandle>::success(found->second);
            }
            auto created = createTexture(SceneTexture{key, srgb, path.empty(), solidColor});
            if (created)
            {
                textureMap_.emplace(mapKey, created.value());
                textureReferences_[created.value()] = 1u;
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
            const auto toByte = [](float value)
            {
                return static_cast<std::uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
            };
            const std::array<std::uint8_t, 4> baseColorValue = {toByte(source.pbr.baseColor.r),
                toByte(source.pbr.baseColor.g),
                toByte(source.pbr.baseColor.b),
                toByte(source.pbr.baseColor.a)};
            const auto baseColor = importTexture(source.baseColorTexture,
                true,
                "__halcyon_default_base_color_srgb__",
                baseColorValue);
            const auto normal = importTexture(
                source.normalTexture, false, "__halcyon_default_normal__", {128, 128, 255, 255});
            const auto metallicRoughness = importTexture(source.metallicRoughnessTexture,
                false,
                "__halcyon_default_black__",
                {0, 0, 0, 255});
            const auto occlusion = importTexture(
                source.occlusionTexture, false, "__halcyon_default_white__", {255, 255, 255, 255});
            const auto emissive = importTexture(
                source.emissiveTexture, true, "__halcyon_default_black_srgb__", {0, 0, 0, 255});
            if (!baseColor || !normal || !metallicRoughness || !occlusion || !emissive)
            {
                rollback(imported);
                return Halcyon::Result<SceneImportResult>::failure(Halcyon::Error{
                    Halcyon::ErrorCode::OutOfMemory, "failed to allocate scene material textures"});
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
                {}, true, "__halcyon_default_base_color_srgb__", {255, 255, 255, 255});
            const auto normal =
                importTexture({}, false, "__halcyon_default_normal__", {128, 128, 255, 255});
            const auto metallicRoughness =
                importTexture({}, false, "__halcyon_default_black__", {0, 0, 0, 255});
            const auto occlusion =
                importTexture({}, false, "__halcyon_default_white__", {255, 255, 255, 255});
            const auto emissive =
                importTexture({}, true, "__halcyon_default_black_srgb__", {0, 0, 0, 255});
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
                rollback(imported);
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
        const bool removed = textures_.erase(handle);
        textureReferences_.erase(handle);
        if (removed)
        {
            for (auto it = textureMap_.begin(); it != textureMap_.end();)
            {
                it = it->second == handle ? textureMap_.erase(it) : std::next(it);
            }
        }
        return removed;
    }
    void clear() noexcept
    {
        meshes_.clear();
        materials_.clear();
        textures_.clear();
        textureMap_.clear();
        textureReferences_.clear();
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
            const auto ref = textureReferences_.find(handle);
            if (ref == textureReferences_.end())
            {
                continue;
            }
            if (ref->second > 1u)
            {
                --ref->second;
            }
            else
            {
                textureReferences_.erase(handle);
                (void)textures_.erase(handle);
            }
        }
        // Texture handles are shared across assets. Remove only cache entries
        // that pointed at records created by this transaction; references from
        // previously committed assets remain valid and continue to deduplicate
        // deterministic default textures.
        for (auto it = textureMap_.begin(); it != textureMap_.end();)
        {
            const bool createdByTransaction = std::find(imported.textures.begin(),
                imported.textures.end(), it->second) != imported.textures.end();
            // A shared texture can be present in imported.textures while a
            // previous asset still owns it. Keep its cache entry in that case.
            it = createdByTransaction && textures_.get(it->second) == nullptr
                     ? textureMap_.erase(it)
                     : std::next(it);
        }
    }

    Core::HandlePool<SceneMesh, Resources::MeshHandle> meshes_;
    Core::HandlePool<SceneMaterial, Resources::MaterialHandle> materials_;
    Core::HandlePool<SceneTexture, Resources::TextureHandle> textures_;
    std::unordered_map<std::string, TextureHandle> textureMap_;
    std::unordered_map<TextureHandle, std::uint32_t> textureReferences_;
};

} // namespace Halcyon::Renderer::Scene
