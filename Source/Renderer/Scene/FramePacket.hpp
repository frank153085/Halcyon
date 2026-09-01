#pragma once

// Backend-neutral per-frame data.  A FramePacket is deliberately a small,
// immutable view over scene data: it contains no Vulkan (or other API)
// handles, which makes it safe to capture, replay and feed to more than one
// renderer implementation.

#include "Camera.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace Halcyon::Renderer::Scene
{

struct alignas(16) InstanceData
{
    // Column-major affine transform.  The final row/column are kept explicit
    // rather than using a glm type so the packet has a stable wire layout.
    std::array<float, 16> transform{};
    std::uint32_t meshId = 0;
    std::uint32_t materialId = 0;
    std::uint32_t flags = 0;
    std::uint32_t _padding = 0;
};

struct alignas(16) LightData
{
    // xyz + radius and rgb + intensity, all in linear Rec.709 space.
    std::array<float, 4> positionAndRadius{};
    std::array<float, 4> colorAndIntensity{};
};

static_assert(alignof(InstanceData) == 16);
static_assert(sizeof(InstanceData) % 16 == 0);
static_assert(std::is_standard_layout_v<InstanceData>);
static_assert(alignof(LightData) == 16);
static_assert(sizeof(LightData) % 16 == 0);
static_assert(std::is_standard_layout_v<LightData>);

struct FramePacket
{
    std::uint64_t frameIndex = 0;
    CameraData camera{};
    std::span<const InstanceData> instances{};
    std::span<const LightData> lights{};
};

// Owning counterpart used by capture/replay and tools.  The returned view is
// valid until either vector is modified or the owning object is destroyed.
struct OwnedFramePacket
{
    std::uint64_t frameIndex = 0;
    CameraData camera{};
    std::vector<InstanceData> instances;
    std::vector<LightData> lights;

    [[nodiscard]] FramePacket view() const noexcept
    {
        return FramePacket{frameIndex, camera, instances, lights};
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return instances.empty() && lights.empty();
    }
};

} // namespace Halcyon::Renderer::Scene

namespace Halcyon
{
using SceneFramePacket = Renderer::Scene::FramePacket;
using OwnedSceneFramePacket = Renderer::Scene::OwnedFramePacket;
} // namespace Halcyon
