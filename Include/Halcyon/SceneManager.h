#pragma once

#include "Core/Handle.h"
#include "Core/Result.h"
#include "Scene.h"

#include <filesystem>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Halcyon
{

namespace Vulkan
{
class Renderer;
}

struct SceneAssetTag
{
};

struct SceneInstanceTag
{
};

using SceneAssetHandle = Core::Handle<SceneAssetTag>;
using SceneInstanceHandle = Core::Handle<SceneInstanceTag>;
using SceneAssetSource = std::variant<std::filesystem::path, StaticScene>;

struct SceneAssetConfig
{
    std::string name;
    SceneAssetSource source;
};

struct SceneInstanceConfig
{
    std::string name;
    std::string assetName;
    glm::mat4 transform{1.0f};
};

struct SceneManagerConfig
{
    std::string name;
    std::filesystem::path assetRoot;
    std::vector<SceneAssetConfig> assets;
    std::vector<SceneInstanceConfig> instances;
};

// Owns the CPU scene, persistent resource database, asset records, and scene
// instances. Render backends receive resource records only through this
// manager; applications never create backend-specific mesh or texture data.
class SceneManager final
{
public:
    SceneManager();
    ~SceneManager();

    SceneManager(const SceneManager&) = delete;
    SceneManager& operator=(const SceneManager&) = delete;
    SceneManager(SceneManager&&) noexcept;
    SceneManager& operator=(SceneManager&&) noexcept;

    [[nodiscard]] Result<SceneAssetHandle> loadAsset(const SceneAssetConfig& config);
    [[nodiscard]] Result<SceneAssetHandle> loadAsset(
        std::string name, const std::filesystem::path& path);
    [[nodiscard]] Result<SceneAssetHandle> createAsset(std::string name, StaticScene sceneAsset);
    [[nodiscard]] Result<SceneInstanceHandle> createInstance(const SceneInstanceConfig& config);
    [[nodiscard]] Result<void> destroyInstance(SceneInstanceHandle instance);
    [[nodiscard]] Result<void> unloadAsset(SceneAssetHandle asset);

    [[nodiscard]] SceneAssetHandle findAsset(std::string_view name) const noexcept;
    [[nodiscard]] SceneInstanceHandle findInstance(std::string_view name) const noexcept;
    [[nodiscard]] Entity rootEntity(SceneInstanceHandle instance) const noexcept;

    [[nodiscard]] Scene& scene() noexcept;
    [[nodiscard]] const Scene& scene() const noexcept;
    [[nodiscard]] const SceneDatabase& database() const noexcept;
    [[nodiscard]] const std::filesystem::path& assetRoot() const noexcept;
    [[nodiscard]] const std::string& name() const noexcept;

    void shutdown() noexcept;

private:
    friend class Engine;
    struct Impl;

    [[nodiscard]] Result<void> initialize(
        const SceneManagerConfig& config, Vulkan::Renderer& renderer);
    [[nodiscard]] Result<OwnedSceneFramePacket> extract(
        const Renderer::Scene::CameraData& camera, std::uint64_t frameIndex) const;

    std::unique_ptr<Impl> impl_;
};

} // namespace Halcyon
