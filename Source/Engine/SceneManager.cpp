#include "Halcyon/SceneManager.h"

#include "Core/HandlePool.h"
#include "Renderer/Scene/Ecs/RenderExtractor.h"
#include "Renderer/Vulkan/HalcyonVulkanRenderer.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <new>
#include <utility>

namespace Halcyon
{
namespace
{

struct AssetRecord
{
    std::string name;
    StaticScene source;
    SceneImportResult imported;
    std::size_t liveInstances = 0;
};

struct InstanceRecord
{
    std::string name;
    SceneAssetHandle asset{};
    Entity root{};
    std::vector<Entity> entities;
};

[[nodiscard]] Error sceneManagerError(ErrorCode code, std::string message)
{
    return MakeError(code, std::move(message), "SceneManager");
}

[[nodiscard]] bool isSupportedScenePath(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(),
        extension.end(),
        extension.begin(),
        [](unsigned char value)
        {
            return static_cast<char>(std::tolower(value));
        });
    return extension == ".gltf" || extension == ".glb";
}

} // namespace

struct SceneManager::Impl
{
    Vulkan::Renderer* renderer = nullptr;
    Scene scene;
    SceneDatabase database;
    Core::HandlePool<AssetRecord, SceneAssetHandle> assets;
    Core::HandlePool<InstanceRecord, SceneInstanceHandle> instances;
    std::map<std::string, SceneAssetHandle, std::less<>> assetNames;
    std::map<std::string, SceneInstanceHandle, std::less<>> instanceNames;
    std::filesystem::path assetRoot;
    std::string name;

    void eraseImported(const SceneImportResult& imported) noexcept
    {
        for (const SceneMeshHandle handle : imported.meshes)
        {
            (void)database.destroy(handle);
        }
        for (const SceneMaterialHandle handle : imported.materials)
        {
            (void)database.destroy(handle);
        }
        database.releaseImportedTextures(imported.textures);
    }

    void destroyEntities(InstanceRecord& record) noexcept
    {
        for (auto entity = record.entities.rbegin(); entity != record.entities.rend(); ++entity)
        {
            scene.destroyEntity(*entity);
        }
        record.entities.clear();
        record.root = Entity::invalid();
    }
};

SceneManager::SceneManager()
        : impl_(std::make_unique<Impl>())
{
}

SceneManager::~SceneManager()
{
    shutdown();
}

SceneManager::SceneManager(SceneManager&&) noexcept = default;
SceneManager& SceneManager::operator=(SceneManager&& other) noexcept
{
    if (this != &other)
    {
        shutdown();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

Result<void> SceneManager::initialize(const SceneManagerConfig& config, Vulkan::Renderer& renderer)
{
    shutdown();
    if (impl_ == nullptr)
    {
        impl_ = std::make_unique<Impl>();
    }
    impl_->renderer = &renderer;
    impl_->assetRoot = config.assetRoot.lexically_normal();
    impl_->name = config.name;

    for (const SceneAssetConfig& asset : config.assets)
    {
        const auto loaded = loadAsset(asset);
        if (!loaded)
        {
            const Error error = loaded.error().withContext("startup scene configuration");
            shutdown();
            return Result<void>::failure(error);
        }
    }
    for (const SceneInstanceConfig& instance : config.instances)
    {
        const auto created = createInstance(instance);
        if (!created)
        {
            const Error error = created.error().withContext("startup scene configuration");
            shutdown();
            return Result<void>::failure(error);
        }
    }
    return Result<void>::success();
}

Result<SceneAssetHandle> SceneManager::loadAsset(const SceneAssetConfig& config)
{
    if (std::holds_alternative<std::filesystem::path>(config.source))
    {
        return loadAsset(config.name, std::get<std::filesystem::path>(config.source));
    }
    try
    {
        return createAsset(config.name, std::get<StaticScene>(config.source));
    }
    catch (const std::bad_alloc&)
    {
        return Result<SceneAssetHandle>::failure(
            sceneManagerError(ErrorCode::OutOfMemory, "failed to copy procedural scene data"));
    }
}

Result<SceneAssetHandle> SceneManager::loadAsset(
    std::string name, const std::filesystem::path& path)
{
    if (impl_ == nullptr)
    {
        return Result<SceneAssetHandle>::failure(
            sceneManagerError(ErrorCode::InvalidState, "scene manager state is unavailable"));
    }
    if (name.empty())
    {
        return Result<SceneAssetHandle>::failure(
            sceneManagerError(ErrorCode::InvalidArgument, "scene asset name is empty"));
    }
    if (impl_->assetNames.contains(name))
    {
        return Result<SceneAssetHandle>::failure(
            sceneManagerError(ErrorCode::AlreadyExists, "scene asset already exists: " + name));
    }
    if (path.empty())
    {
        return Result<SceneAssetHandle>::failure(
            sceneManagerError(ErrorCode::InvalidArgument, "scene asset path is empty"));
    }
    if (path.is_relative() && impl_->assetRoot.empty())
    {
        return Result<SceneAssetHandle>::failure(sceneManagerError(ErrorCode::InvalidArgument,
            "relative scene asset path requires a configured asset root: " + path.string()));
    }

    std::filesystem::path resolved = path.is_absolute() ? path : impl_->assetRoot / path;
    std::error_code pathError;
    resolved = std::filesystem::absolute(resolved, pathError).lexically_normal();
    if (pathError || !std::filesystem::is_regular_file(resolved, pathError))
    {
        return Result<SceneAssetHandle>::failure(sceneManagerError(
            ErrorCode::NotFound, "scene asset does not exist: " + resolved.string()));
    }
    if (!isSupportedScenePath(resolved))
    {
        return Result<SceneAssetHandle>::failure(sceneManagerError(
            ErrorCode::Unsupported, "scene assets must use .gltf or .glb: " + resolved.string()));
    }
    auto loaded = Renderer::Scene::loadStaticScene(resolved);
    if (!loaded)
    {
        return Result<SceneAssetHandle>::failure(
            loaded.error().withContext("SceneManager::loadAsset " + resolved.string()));
    }
    return createAsset(std::move(name), std::move(loaded).value());
}

Result<SceneAssetHandle> SceneManager::createAsset(std::string name, StaticScene sceneAsset)
{
    if (impl_ == nullptr)
    {
        return Result<SceneAssetHandle>::failure(
            sceneManagerError(ErrorCode::InvalidState, "scene manager state is unavailable"));
    }
    if (name.empty())
    {
        return Result<SceneAssetHandle>::failure(
            sceneManagerError(ErrorCode::InvalidArgument, "scene asset name is empty"));
    }
    if (impl_->assetNames.contains(name))
    {
        return Result<SceneAssetHandle>::failure(
            sceneManagerError(ErrorCode::AlreadyExists, "scene asset already exists: " + name));
    }
    if (sceneAsset.primitives.empty())
    {
        return Result<SceneAssetHandle>::failure(sceneManagerError(
            ErrorCode::InvalidArgument, "scene asset has no primitives: " + name));
    }

    const auto importedResult = impl_->database.import(sceneAsset);
    if (!importedResult)
    {
        return Result<SceneAssetHandle>::failure(
            importedResult.error().withContext("SceneManager::createAsset " + name));
    }
    SceneImportResult imported = importedResult.value();
    if (impl_->renderer != nullptr)
    {
        const auto upload = impl_->renderer->uploadSceneAsset(impl_->database, imported);
        if (!upload)
        {
            impl_->eraseImported(imported);
            return Result<SceneAssetHandle>::failure(
                upload.error().withContext("SceneManager::createAsset " + name));
        }
    }

    const auto created =
        impl_->assets.tryEmplace(AssetRecord{name, std::move(sceneAsset), imported, 0});
    if (!created)
    {
        if (impl_->renderer != nullptr)
        {
            (void)impl_->renderer->releaseSceneAsset(imported);
        }
        impl_->eraseImported(imported);
        return Result<SceneAssetHandle>::failure(created.error().withContext("SceneManager"));
    }
    try
    {
        impl_->assetNames.emplace(name, created.value());
    }
    catch (const std::bad_alloc&)
    {
        (void)impl_->assets.erase(created.value());
        if (impl_->renderer != nullptr)
        {
            (void)impl_->renderer->releaseSceneAsset(imported);
        }
        impl_->eraseImported(imported);
        return Result<SceneAssetHandle>::failure(
            sceneManagerError(ErrorCode::OutOfMemory, "failed to index scene asset name"));
    }
    if (impl_->renderer != nullptr)
    {
        impl_->renderer->invalidateTaaHistory();
    }
    return created;
}

Result<SceneInstanceHandle> SceneManager::createInstance(const SceneInstanceConfig& config)
{
    if (impl_ == nullptr)
    {
        return Result<SceneInstanceHandle>::failure(
            sceneManagerError(ErrorCode::InvalidState, "scene manager state is unavailable"));
    }
    if (config.name.empty() || config.assetName.empty())
    {
        return Result<SceneInstanceHandle>::failure(sceneManagerError(
            ErrorCode::InvalidArgument, "scene instance and asset names must not be empty"));
    }
    if (impl_->instanceNames.contains(config.name))
    {
        return Result<SceneInstanceHandle>::failure(sceneManagerError(
            ErrorCode::AlreadyExists, "scene instance already exists: " + config.name));
    }
    const SceneAssetHandle assetHandle = findAsset(config.assetName);
    AssetRecord* asset = impl_->assets.get(assetHandle);
    if (asset == nullptr)
    {
        return Result<SceneInstanceHandle>::failure(sceneManagerError(ErrorCode::NotFound,
            "scene instance references an unknown asset: " + config.assetName));
    }

    InstanceRecord record;
    record.name = config.name;
    record.asset = assetHandle;
    try
    {
        // Reserve all entity slots before creating the root so an allocation
        // failure cannot leave an untracked ECS entity behind.
        record.entities.reserve(1u + asset->source.nodes.size() + asset->source.primitives.size());
        record.root = impl_->scene.createEntity();
        if (!record.root.isValid())
        {
            return Result<SceneInstanceHandle>::failure(
                sceneManagerError(ErrorCode::OutOfMemory, "failed to create instance root entity"));
        }
        record.entities.push_back(record.root);
        TransformComponent rootTransform{};
        rootTransform.localTransform = config.transform;
        (void)impl_->scene.transforms().add(record.root, rootTransform);

        std::vector<Entity> nodeEntities(asset->source.nodes.size(), Entity::invalid());
        for (std::size_t i = 0; i < asset->source.nodes.size(); ++i)
        {
            const Entity entity = impl_->scene.createEntity();
            nodeEntities[i] = entity;
            record.entities.push_back(entity);
            TransformComponent transform{};
            transform.localTransform = asset->source.nodes[i].localTransform;
            transform.parent = record.root;
            (void)impl_->scene.transforms().add(entity, transform);
        }
        for (std::size_t i = 0; i < asset->source.nodes.size(); ++i)
        {
            const std::int32_t parent = asset->source.nodes[i].parent;
            if (parent >= 0 && static_cast<std::size_t>(parent) < nodeEntities.size())
            {
                if (TransformComponent* transform = impl_->scene.transforms().get(nodeEntities[i]))
                {
                    transform->parent = nodeEntities[static_cast<std::size_t>(parent)];
                }
            }
        }

        std::vector<std::int32_t> primitiveOwners(asset->source.primitives.size(), -1);
        for (std::size_t nodeIndex = 0; nodeIndex < asset->source.nodes.size(); ++nodeIndex)
        {
            for (const std::uint32_t primitive : asset->source.nodes[nodeIndex].primitiveIndices)
            {
                if (primitive < primitiveOwners.size() && primitiveOwners[primitive] < 0)
                {
                    primitiveOwners[primitive] = static_cast<std::int32_t>(nodeIndex);
                }
            }
        }

        for (std::size_t i = 0; i < asset->source.primitives.size(); ++i)
        {
            const auto& primitive = asset->source.primitives[i];
            const Entity entity = impl_->scene.createEntity();
            record.entities.push_back(entity);
            TransformComponent transform{};
            const std::int32_t owner = primitiveOwners[i];
            if (owner >= 0 && static_cast<std::size_t>(owner) < nodeEntities.size())
            {
                transform.parent = nodeEntities[static_cast<std::size_t>(owner)];
            }
            else
            {
                transform.parent = record.root;
                transform.localTransform = primitive.worldTransform;
            }
            (void)impl_->scene.transforms().add(entity, transform);

            RenderableComponent renderable{};
            if (i < asset->imported.meshes.size())
            {
                renderable.mesh = asset->imported.meshes[i];
            }
            const std::uint32_t materialIndex = primitive.materialIndex;
            if (materialIndex < asset->imported.materials.size())
            {
                renderable.material = asset->imported.materials[materialIndex];
            }
            else if (!asset->imported.materials.empty())
            {
                renderable.material = asset->imported.materials.front();
            }
            renderable.flags = static_cast<std::uint32_t>(RenderableFlags::CastShadow) |
                               static_cast<std::uint32_t>(RenderableFlags::ReceiveShadow);
            if (materialIndex < asset->source.materials.size())
            {
                const auto& material = asset->source.materials[materialIndex];
                if (material.transparent)
                {
                    renderable.flags |= static_cast<std::uint32_t>(RenderableFlags::Transparent);
                }
                if (material.doubleSided)
                {
                    renderable.flags |= static_cast<std::uint32_t>(RenderableFlags::DoubleSided);
                }
                if (material.alphaMasked)
                {
                    renderable.flags |= static_cast<std::uint32_t>(RenderableFlags::AlphaMasked);
                }
            }
            (void)impl_->scene.renderables().add(entity, renderable);
        }

        const auto created = impl_->instances.tryEmplace(std::move(record));
        if (!created)
        {
            impl_->destroyEntities(record);
            return Result<SceneInstanceHandle>::failure(
                created.error().withContext("SceneManager"));
        }
        try
        {
            impl_->instanceNames.emplace(config.name, created.value());
        }
        catch (const std::bad_alloc&)
        {
            if (InstanceRecord* stored = impl_->instances.get(created.value()))
            {
                impl_->destroyEntities(*stored);
            }
            (void)impl_->instances.erase(created.value());
            return Result<SceneInstanceHandle>::failure(
                sceneManagerError(ErrorCode::OutOfMemory, "failed to index scene instance name"));
        }
        ++asset->liveInstances;
        if (impl_->renderer != nullptr)
        {
            impl_->renderer->invalidateTaaHistory();
        }
        return created;
    }
    catch (const std::bad_alloc&)
    {
        impl_->destroyEntities(record);
        return Result<SceneInstanceHandle>::failure(
            sceneManagerError(ErrorCode::OutOfMemory, "failed to create scene instance"));
    }
}

Result<void> SceneManager::destroyInstance(SceneInstanceHandle instance)
{
    if (impl_ == nullptr)
    {
        return Result<void>::failure(
            sceneManagerError(ErrorCode::InvalidState, "scene manager is not initialized"));
    }
    InstanceRecord* record = impl_->instances.get(instance);
    if (record == nullptr)
    {
        return Result<void>::failure(
            sceneManagerError(ErrorCode::NotFound, "scene instance handle is invalid"));
    }
    if (AssetRecord* asset = impl_->assets.get(record->asset);
        asset != nullptr && asset->liveInstances > 0)
    {
        --asset->liveInstances;
    }
    impl_->instanceNames.erase(record->name);
    impl_->destroyEntities(*record);
    (void)impl_->instances.erase(instance);
    if (impl_->renderer != nullptr)
    {
        impl_->renderer->invalidateTaaHistory();
    }
    return Result<void>::success();
}

Result<void> SceneManager::unloadAsset(SceneAssetHandle asset)
{
    if (impl_ == nullptr)
    {
        return Result<void>::failure(
            sceneManagerError(ErrorCode::InvalidState, "scene manager state is unavailable"));
    }
    AssetRecord* record = impl_->assets.get(asset);
    if (record == nullptr)
    {
        return Result<void>::failure(
            sceneManagerError(ErrorCode::NotFound, "scene asset handle is invalid"));
    }
    if (record->liveInstances != 0)
    {
        return Result<void>::failure(sceneManagerError(
            ErrorCode::InvalidState, "scene asset still has live instances: " + record->name));
    }
    if (impl_->renderer != nullptr)
    {
        const auto released = impl_->renderer->releaseSceneAsset(record->imported);
        if (!released)
        {
            return Result<void>::failure(released.error().withContext("SceneManager::unloadAsset"));
        }
    }
    impl_->eraseImported(record->imported);
    impl_->assetNames.erase(record->name);
    (void)impl_->assets.erase(asset);
    if (impl_->renderer != nullptr)
    {
        impl_->renderer->invalidateTaaHistory();
    }
    return Result<void>::success();
}

SceneAssetHandle SceneManager::findAsset(std::string_view name) const noexcept
{
    if (impl_ == nullptr)
    {
        return SceneAssetHandle::invalid();
    }
    const auto found = impl_->assetNames.find(name);
    return found != impl_->assetNames.end() ? found->second : SceneAssetHandle::invalid();
}

SceneInstanceHandle SceneManager::findInstance(std::string_view name) const noexcept
{
    if (impl_ == nullptr)
    {
        return SceneInstanceHandle::invalid();
    }
    const auto found = impl_->instanceNames.find(name);
    return found != impl_->instanceNames.end() ? found->second : SceneInstanceHandle::invalid();
}

Entity SceneManager::rootEntity(SceneInstanceHandle instance) const noexcept
{
    if (impl_ == nullptr)
    {
        return Entity::invalid();
    }
    const InstanceRecord* record = impl_->instances.get(instance);
    return record != nullptr ? record->root : Entity::invalid();
}

Scene& SceneManager::scene() noexcept
{
    static Scene empty;
    return impl_ != nullptr ? impl_->scene : empty;
}

const Scene& SceneManager::scene() const noexcept
{
    static const Scene empty;
    return impl_ != nullptr ? impl_->scene : empty;
}

const SceneDatabase& SceneManager::database() const noexcept
{
    static const SceneDatabase empty;
    return impl_ != nullptr ? impl_->database : empty;
}

const std::filesystem::path& SceneManager::assetRoot() const noexcept
{
    static const std::filesystem::path empty;
    return impl_ != nullptr ? impl_->assetRoot : empty;
}

const std::string& SceneManager::name() const noexcept
{
    static const std::string empty;
    return impl_ != nullptr ? impl_->name : empty;
}

OwnedSceneFramePacket SceneManager::extract(
    const Renderer::Scene::CameraData& camera, std::uint64_t frameIndex) const
{
    return Renderer::Scene::Ecs::RenderExtractor::extract(scene(), camera, frameIndex);
}

void SceneManager::shutdown() noexcept
{
    if (impl_ == nullptr)
    {
        return;
    }
    while (!impl_->instanceNames.empty())
    {
        (void)destroyInstance(impl_->instanceNames.begin()->second);
    }

    while (!impl_->assetNames.empty())
    {
        const SceneAssetHandle handle = impl_->assetNames.begin()->second;
        AssetRecord* record = impl_->assets.get(handle);
        if (record == nullptr)
        {
            impl_->assetNames.erase(impl_->assetNames.begin());
            continue;
        }
        if (impl_->renderer != nullptr)
        {
            (void)impl_->renderer->releaseSceneAsset(record->imported);
        }
        impl_->eraseImported(record->imported);
        impl_->assetNames.erase(record->name);
        (void)impl_->assets.erase(handle);
    }
    impl_->instanceNames.clear();
    impl_->scene.clear();
    impl_->database.clear();
    impl_->assetRoot.clear();
    impl_->name.clear();
    impl_->renderer = nullptr;
}

} // namespace Halcyon
