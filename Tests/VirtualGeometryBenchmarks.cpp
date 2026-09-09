#include "Renderer/Scene/Sha256.h"
#include "Renderer/Scene/VirtualGeometry.h"
#include "Renderer/Scene/VirtualGeometryCache.h"

#include <benchmark/benchmark.h>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <utility>

using namespace Halcyon::Renderer::Scene;

namespace
{
StaticScene makeGrid(std::uint32_t width)
{
    StaticScene scene; scene.materials.emplace_back(); StaticScenePrimitive primitive;
    primitive.vertices.reserve(static_cast<std::size_t>(width + 1) * (width + 1));
    for (std::uint32_t y = 0; y <= width; ++y)
        for (std::uint32_t x = 0; x <= width; ++x)
            primitive.vertices.push_back({{static_cast<float>(x), static_cast<float>(y), 0}, {0, 0, 1}});
    for (std::uint32_t y = 0; y < width; ++y) for (std::uint32_t x = 0; x < width; ++x) {
        const std::uint32_t a = y * (width + 1) + x, b = a + 1, c = a + width + 1, d = c + 1;
        primitive.indices.insert(primitive.indices.end(), {a, b, d, a, d, c});
    }
    primitive.boundsMax = {static_cast<float>(width), static_cast<float>(width), 0};
    scene.primitives.push_back(std::move(primitive)); return scene;
}

void buildMeshlets(benchmark::State& state)
{
    const auto scene = makeGrid(static_cast<std::uint32_t>(state.range(0)));
    for (auto _ : state) { auto result = buildVirtualGeometry(scene); benchmark::DoNotOptimize(result); }
}
BENCHMARK(buildMeshlets)->Arg(32)->Arg(128)->Unit(benchmark::kMillisecond);

void cacheRead(benchmark::State& state)
{
    const auto asset = buildVirtualGeometry(makeGrid(64)).value();
    const Sha256Digest hash{};
    const auto path = std::filesystem::temp_directory_path() / "halcyon-vg-benchmark.halcyon.vgcache";
    (void)writeVirtualGeometryCache(path, asset, hash);
    for (auto _ : state) { auto result = readVirtualGeometryCache(path, &hash); benchmark::DoNotOptimize(result); }
    std::error_code error; std::filesystem::remove(path, error);
}
BENCHMARK(cacheRead)->Unit(benchmark::kMillisecond);

void cacheWrite(benchmark::State& state)
{
    const auto asset = buildVirtualGeometry(makeGrid(64)).value();
    const Sha256Digest hash{};
    const auto path = std::filesystem::temp_directory_path() /
        "halcyon-vg-write-benchmark.halcyon.vgcache";
    for (auto _ : state)
    {
        const auto result = writeVirtualGeometryCache(path, asset, hash);
        benchmark::DoNotOptimize(result);
        if (!result)
        {
            state.SkipWithError(result.error().message.c_str());
            break;
        }
    }
    std::error_code error;
    std::filesystem::remove(path, error);
}
BENCHMARK(cacheWrite)->Unit(benchmark::kMillisecond);

std::filesystem::path lucyPath()
{
    if (const char* configured = std::getenv("HALCYON_LUCY_PLY"); configured != nullptr &&
        *configured != '\0')
        return std::filesystem::path(configured);
#ifdef HALCYON_SOURCE_DIR
    return std::filesystem::path(HALCYON_SOURCE_DIR) / "assets/models/lucy/lucy.ply";
#else
    return std::filesystem::path("assets/models/lucy/lucy.ply");
#endif
}

std::optional<StaticScene> loadLucyForBenchmark(benchmark::State& state)
{
    const auto path = lucyPath();
    if (!std::filesystem::is_regular_file(path))
    {
        state.SkipWithError("Lucy PLY is unavailable; set HALCYON_LUCY_PLY to enable this benchmark");
        return std::nullopt;
    }
    const auto loaded = loadGeometrySource(path);
    if (!loaded)
    {
        state.SkipWithError(loaded.error().message.c_str());
        return std::nullopt;
    }
    return std::move(loaded).value();
}

void lucyMeshlets(benchmark::State& state)
{
    auto scene = loadLucyForBenchmark(state);
    if (!scene) return;
    for (auto _ : state)
    {
        auto result = buildVirtualGeometry(*scene);
        benchmark::DoNotOptimize(result);
        if (!result)
        {
            state.SkipWithError(result.error().message.c_str());
            return;
        }
    }
}
BENCHMARK(lucyMeshlets)->Unit(benchmark::kMillisecond);

void lucyCacheRoundTrip(benchmark::State& state)
{
    auto scene = loadLucyForBenchmark(state);
    if (!scene) return;
    const auto built = buildVirtualGeometry(*scene);
    if (!built)
    {
        state.SkipWithError(built.error().message.c_str());
        return;
    }
    const auto path = std::filesystem::temp_directory_path() /
        "halcyon-lucy-vg-benchmark.halcyon.vgcache";
    const Sha256Digest hash{};
    if (!writeVirtualGeometryCache(path, built.value(), hash))
    {
        state.SkipWithError("unable to create Lucy benchmark cache");
        return;
    }
    for (auto _ : state)
    {
        auto result = readVirtualGeometryCache(path, &hash);
        benchmark::DoNotOptimize(result);
        if (!result)
        {
            state.SkipWithError(result.error().message.c_str());
            break;
        }
    }
    std::error_code error;
    std::filesystem::remove(path, error);
}
BENCHMARK(lucyCacheRoundTrip)->Unit(benchmark::kMillisecond);
} // namespace

BENCHMARK_MAIN();
