#include "Renderer/Scene/StaticSceneLoader.h"

#include <cassert>
#include <filesystem>
#include <iostream>

#ifndef HALCYON_SOURCE_DIR
#define HALCYON_SOURCE_DIR "."
#endif

int main()
{
    const std::filesystem::path root = HALCYON_SOURCE_DIR;
    const auto helmet = root / "assets/m3/DamagedHelmet.glb";
    const auto sponza = root / "assets/m3/Sponza/Sponza.gltf";
    for (const auto& path : {helmet, sponza})
    {
        if (!std::filesystem::exists(path)) continue;
        const auto result = Halcyon::Renderer::Scene::loadStaticScene(path);
        if (!result)
        {
            std::cerr << result.error().describe() << '\n';
            return 1;
        }
        const auto& scene = result.value();
        assert(!scene.primitives.empty());
        assert(!scene.materials.empty());
        for (const auto& primitive : scene.primitives)
        {
            assert(!primitive.vertices.empty());
            assert(primitive.indices.size() % 3u == 0u);
        }
    }
    return 0;
}
