#include "Renderer/Quality/Quality.h"

#include <cmath>
#include <iostream>

namespace
{

struct Context
{
    int failures = 0;
    void expect(bool condition, const char* expression, int line)
    {
        if (!condition)
        {
            ++failures;
            std::cerr << "FAILED line " << line << ": " << expression << '\n';
        }
    }
};

#define EXPECT(context, expression)                                                                \
    (context).expect(static_cast<bool>(expression), #expression, __LINE__)

} // namespace

int main()
{
    Context context;
    using namespace Halcyon::Renderer::Quality;

    PbrInput pbr;
    pbr.material.baseColor = {0.8f, 0.2f, 0.1f, 1.0f};
    pbr.material.metallic = 0.5f;
    pbr.material.roughness = 0.4f;
    const glm::vec3 direct = evaluateDirectPbr(pbr, PbrLight{});
    EXPECT(context, std::isfinite(direct.x) && direct.x >= 0.0f);

    const glm::vec3 normal{0.2f, -0.6f, 0.77f};
    const PackedGBuffer packed = packGBuffer(GBufferPixel{
        normal, {0.2f, 0.4f, 0.8f, 1.0f}, 0.7f, 0.3f, 0.9f, {0.1f, 0.2f, 0.3f}, {0.01f, -0.02f}});
    const GBufferPixel unpacked = unpackGBuffer(packed);
    EXPECT(context, glm::dot(glm::normalize(normal), unpacked.normal) > 0.999f);
    EXPECT(context,
        glm::dot(glm::vec3{0.0f, 0.0f, -1.0f},
            unpackGBuffer(packGBuffer(GBufferPixel{{0.0f, 0.0f, -1.0f}})).normal) > 0.999f);
    EXPECT(context, std::abs(unpacked.metallic - 0.7f) < 0.01f);

    CsmConfig csm;
    csm.cascadeCount = 4;
    const auto splits = computeCascadeSplits(csm);
    EXPECT(context,
        splits.size() == 4u && splits.front() > csm.nearPlane && splits.back() == csm.farPlane);
    const auto cascades = buildCascades(csm, glm::mat4{1.0f}, glm::mat4{1.0f}, {0.2f, -1.0f, 0.1f});
    EXPECT(context, cascades.size() == csm.cascadeCount);

    ClusterGrid grid;
    grid.tilesX = 4;
    grid.tilesY = 2;
    grid.slicesZ = 4;
    const auto clusters = assignClusteredLights(grid,
        glm::mat4{1.0f},
        glm::mat4{1.0f},
        {64u, 32u},
        {ClusterLight{{0.0f, 0.0f, -2.0f}, 1.0f, 3u}});
    bool foundLight = false;
    for (const auto& list : clusters.lights)
    {
        foundLight = foundLight || (!list.empty() && list.front() == 3u);
    }
    EXPECT(context, foundLight);

    EXPECT(context, acesTonemap(glm::vec3{0.0f}) == glm::vec3{0.0f});
    const glm::vec3 taa =
        resolveTemporal(TemporalSample{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {}, true});
    EXPECT(context, taa.x > 0.0f && taa.y > 0.0f);

    if (context.failures != 0)
    {
        return 1;
    }
    std::cout << "All M3 quality tests passed\n";
    return 0;
}
