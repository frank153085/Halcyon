#include "Renderer/Scene/Camera.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <glm/geometric.hpp>
#include <iostream>
#include <limits>
#include <string_view>

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

[[nodiscard]] bool nearlyEqual(float lhs, float rhs, float epsilon = 1.0e-5f) noexcept
{
    return std::abs(lhs - rhs) <= epsilon;
}

[[nodiscard]] bool nearlyEqual(
    const glm::vec3& lhs, const glm::vec3& rhs, float epsilon = 1.0e-5f) noexcept
{
    return glm::length(lhs - rhs) <= epsilon;
}

[[nodiscard]] float projectedDepth(const glm::mat4& projection, float viewDistance)
{
    const glm::vec4 clip = projection * glm::vec4{0.0f, 0.0f, -viewDistance, 1.0f};
    return clip.z / clip.w;
}

void finiteProjectionTests(TestContext& context)
{
    using namespace Halcyon::Renderer::Scene;
    Perspective perspective;
    perspective.verticalFovRadians = glm::radians(90.0f);
    perspective.aspectRatio = 2.0f;
    perspective.nearPlane = 0.25f;
    perspective.farPlane = 250.0f;

    const auto projectionResult = makeReversedZProjection(perspective);
    HALCYON_EXPECT(context, projectionResult);
    if (!projectionResult)
    {
        return;
    }
    const glm::mat4 projection = projectionResult.value();
    HALCYON_EXPECT(context, nearlyEqual(projectedDepth(projection, perspective.nearPlane), 1.0f));
    HALCYON_EXPECT(context, nearlyEqual(projectedDepth(projection, perspective.farPlane), 0.0f));

    const float halfHeight = perspective.nearPlane;
    const float halfWidth = halfHeight * perspective.aspectRatio;
    const glm::vec4 top = projection * glm::vec4{0.0f, halfHeight, -perspective.nearPlane, 1.0f};
    const glm::vec4 right = projection * glm::vec4{halfWidth, 0.0f, -perspective.nearPlane, 1.0f};
    HALCYON_EXPECT(context, nearlyEqual(top.y / top.w, -1.0f));
    HALCYON_EXPECT(context, nearlyEqual(right.x / right.w, 1.0f));
}

void infiniteProjectionAndValidationTests(TestContext& context)
{
    using namespace Halcyon::Renderer::Scene;
    Perspective perspective;
    perspective.nearPlane = 0.1f;
    const auto projectionResult = makeReversedZProjection(perspective);
    HALCYON_EXPECT(context, projectionResult);
    if (projectionResult)
    {
        HALCYON_EXPECT(context, nearlyEqual(projectedDepth(projectionResult.value(), 0.1f), 1.0f));
        HALCYON_EXPECT(context, projectedDepth(projectionResult.value(), 1.0e7f) < 1.0e-7f);
    }

    Perspective invalid = perspective;
    invalid.nearPlane = 0.0f;
    HALCYON_EXPECT(context, !makeReversedZProjection(invalid));
    invalid = perspective;
    invalid.farPlane = perspective.nearPlane;
    HALCYON_EXPECT(context, !makeReversedZProjection(invalid));
    invalid = perspective;
    invalid.aspectRatio = std::numeric_limits<float>::quiet_NaN();
    HALCYON_EXPECT(context, !makeReversedZProjection(invalid));
}

void viewAndGpuDataTests(TestContext& context)
{
    using namespace Halcyon::Renderer::Scene;
    Camera camera;
    HALCYON_EXPECT(context, camera.lookAt({1.0f, 2.0f, 3.0f}, {1.0f, 2.0f, 2.0f}));
    HALCYON_EXPECT(context, nearlyEqual(camera.forward(), glm::vec3{0.0f, 0.0f, -1.0f}));
    HALCYON_EXPECT(context, nearlyEqual(camera.up(), glm::vec3{0.0f, 1.0f, 0.0f}));

    const glm::mat4 view = camera.viewMatrix();
    const glm::vec4 cameraOrigin = view * glm::vec4{camera.position(), 1.0f};
    const glm::vec4 oneMeterForward = view * glm::vec4{camera.position() + camera.forward(), 1.0f};
    HALCYON_EXPECT(context, nearlyEqual(glm::vec3{cameraOrigin}, glm::vec3{0.0f}));
    HALCYON_EXPECT(context, nearlyEqual(glm::vec3{oneMeterForward}, glm::vec3{0.0f, 0.0f, -1.0f}));

    HALCYON_EXPECT(context, camera.setViewport({1920, 1080}));
    const CameraData data = camera.data();
    HALCYON_EXPECT(context, nearlyEqual(data.viewportAndInvViewport.x, 1920.0f));
    HALCYON_EXPECT(context, nearlyEqual(data.viewportAndInvViewport.y, 1080.0f));
    HALCYON_EXPECT(context, nearlyEqual(data.positionAndNear.w, camera.perspective().nearPlane));
    HALCYON_EXPECT(context, nearlyEqual(data.forwardAndFar.w, 0.0f));

    const auto address = reinterpret_cast<std::uintptr_t>(&data);
    HALCYON_EXPECT(context, address % 16u == 0u);
    HALCYON_EXPECT(context, offsetof(CameraData, view) % 16u == 0u);
    HALCYON_EXPECT(context, offsetof(CameraData, projection) % 16u == 0u);
    HALCYON_EXPECT(context, offsetof(CameraData, viewProjection) % 16u == 0u);
    HALCYON_EXPECT(context, offsetof(CameraData, inverseViewProjection) % 16u == 0u);
    HALCYON_EXPECT(context, offsetof(CameraData, positionAndNear) % 16u == 0u);
    HALCYON_EXPECT(context, offsetof(CameraData, forwardAndFar) % 16u == 0u);
}

void failureDoesNotMutateTests(TestContext& context)
{
    using namespace Halcyon::Renderer::Scene;
    Camera camera;
    const Perspective original = camera.perspective();
    Perspective invalid = original;
    invalid.verticalFovRadians = 0.0f;
    HALCYON_EXPECT(context, !camera.setPerspective(invalid));
    HALCYON_EXPECT(
        context, nearlyEqual(camera.perspective().verticalFovRadians, original.verticalFovRadians));
    HALCYON_EXPECT(context, !camera.setViewport({0, 720}));
    HALCYON_EXPECT(context, camera.viewport().width == 1280u);
    HALCYON_EXPECT(context, !camera.lookAt({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}));
    HALCYON_EXPECT(context, !camera.setOrientation(glm::quat{0.0f, 0.0f, 0.0f, 0.0f}));
}

} // namespace

int main()
{
    TestContext context;
    finiteProjectionTests(context);
    infiniteProjectionAndValidationTests(context);
    viewAndGpuDataTests(context);
    failureDoesNotMutateTests(context);

    if (context.failures() != 0)
    {
        std::cerr << context.failures() << " camera test(s) failed\n";
        return 1;
    }
    std::cout << "All camera tests passed\n";
    return 0;
}
