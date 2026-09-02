#pragma once
#include "Halcyon/FrameGraphTypes.h"
namespace Halcyon::Renderer::Graph
{
struct VirtualResource
{
    ResourceKind kind = ResourceKind::Buffer;
    bool imported = false;
    bool transient = true;
    FrameGraphNativeResource native{};
    std::int32_t firstPass = -1;
    std::int32_t lastPass = -1;
};
} // namespace Halcyon::Renderer::Graph
