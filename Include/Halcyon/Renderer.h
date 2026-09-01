#pragma once

// Stable public entry point for the renderer.  Backend implementation details
// remain in the Vulkan module; this facade only re-exports the small API needed
// by applications and tools.

#include "Renderer/Vulkan/HalcyonVulkanRenderer.h"

namespace Halcyon::Public
{

using Renderer = Vulkan::Renderer;
using RendererConfig = Vulkan::RendererConfig;
using FramePacket = Vulkan::FramePacket;
using FrameStats = Vulkan::FrameStats;
using Capabilities = Vulkan::Capabilities;
using Extent2D = Vulkan::Extent2D;
using FeatureMode = Vulkan::FeatureMode;

} // namespace Halcyon::Public
