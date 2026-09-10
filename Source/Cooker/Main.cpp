#include "Renderer/Scene/Sha256.h"
#include "Renderer/Scene/VirtualGeometry.h"
#include "Renderer/Scene/VirtualGeometryCache.h"

#include <filesystem>
#include <iostream>
#include <string_view>

namespace fs = std::filesystem;
using namespace Halcyon::Renderer::Scene;

namespace
{
void usage()
{
    std::cout << "HalcyonCooker --input <scene.gltf|scene.glb|scene.ply> --output <cache.halcyon.vgcache>\n"
                 "              [--lod-count 1..4] [--max-vertices N] [--max-triangles N]\n";
}
}

int main(int argc, char** argv)
{
    fs::path input; fs::path output; VirtualGeometryCacheOptions options;
    std::size_t lodCount = options.build.lodRatios.size();
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view argument = argv[i] != nullptr ? argv[i] : "";
        if (argument == "--help" || argument == "-h") { usage(); return 0; }
        auto value = [&]() -> std::string_view { return i + 1 < argc ? (argv[++i] != nullptr ? argv[i] : "") : std::string_view{}; };
        try
        {
            if (argument == "--input" || argument == "-i") { input = value(); continue; }
            if (argument == "--output" || argument == "-o") { output = value(); continue; }
            if (argument == "--lod-count") { lodCount = std::stoul(std::string(value())); continue; }
            if (argument == "--max-vertices") { options.build.maxVertices = std::stoul(std::string(value())); continue; }
            if (argument == "--max-triangles") { options.build.maxTriangles = std::stoul(std::string(value())); continue; }
        }
        catch (...) { std::cerr << "Invalid value for " << argument << '\n'; return 2; }
        std::cerr << "Unknown or incomplete option: " << argument << '\n'; usage(); return 2;
    }
    if (input.empty() || output.empty() || lodCount == 0 || lodCount > options.build.lodRatios.size()) { usage(); return 2; }
    for (std::size_t i = lodCount; i < options.build.lodRatios.size(); ++i) options.build.lodRatios[i] = 0.0f;
    std::cout << "Hashing source...\n" << std::flush;
    const auto hash = sha256File(input); if (!hash) { std::cerr << hash.error().describe() << '\n'; return 1; }
    std::cout << "Loading source geometry...\n" << std::flush;
    const auto source = loadGeometrySource(input); if (!source) { std::cerr << source.error().describe() << '\n'; return 1; }
    std::cout << "Building deterministic clusters and LOD DAG...\n" << std::flush;
    const auto geometry = buildVirtualGeometry(source.value(), options.build); if (!geometry) { std::cerr << geometry.error().describe() << '\n'; return 1; }
    std::cout << "Writing validated v5 paged cache...\n" << std::flush;
    const auto result = writeVirtualGeometryCache(output, geometry.value(), hash.value(), options); if (!result) { std::cerr << result.error().describe() << '\n'; return 1; }
    std::cout << "Cooked " << geometry.value().triangleCount() << " triangles into " << output.string() << " (" << geometry.value().meshlets.size() << " meshlets)\n";
    return 0;
}
