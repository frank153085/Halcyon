# Halcyon

[Project Roadmap](./ROADMAP.md)

Halcyon is a real-time renderer built for individual study. The repository
now provides the first, intentionally small M2 infrastructure increment. The
M0/M1 Vulkan vertical slice remains the correctness baseline while render
graph, bindless, shader, and profiling foundations stay independently usable.

## Current Milestone: M2 first version (complete)

- Reproducible Debug and RelWithDebInfo presets for MSVC v143 and Ninja.
- Separate `HalcyonCore`, `HalcyonRenderer`, `HalcyonEngine`,
  `HalcyonApplication`, `HalcyonSandbox`, and `HalcyonCooker` targets.
- Vulkan 1.3 instance and device selection, Validation Messenger, Dynamic
  Rendering, Synchronization2, and Timeline Semaphores.
- Three frame contexts, swapchain recreation, and safe minimize, resize, and
  out-of-date handling.
- A reversed-Z camera convention with D32 depth and `GREATER_OR_EQUAL`.
- Build-time HLSL-to-SPIR-V validation through DXC, with embedded triangle
  shaders retained as an offline fallback.
- CPU-side tests for generation handles, deferred deletion, and the upload
  ring foundation.
- CPU render-graph compilation/execution, barrier planning, bindless slot
  lifetime tracking, and frame-budget infrastructure.
- A deliberately thin first-version scene ECS: generation-checked entities,
  dense transform/renderable/light component stores, and extraction into an
  immutable `FramePacket`. The renderer remains independent of ECS policy.
- A Vulkan bindless descriptor-set companion that maps typed CPU slots to
  sampled/storage image, sampler, uniform-buffer, and storage-buffer arrays.
- Renderer-owned bindless slot collection is synchronized with the frame
  timeline when descriptor indexing is available.
- Shader module validation with an optional SPIR-V Tools pass, lightweight
  descriptor reflection, and a reload-safe replacement API for development
  builds.
- Optional Dear ImGui diagnostics overlay; enable it with
  `-DHALCYON_ENABLE_IMGUI=ON`.
- Optional GoogleTest coverage; enable it with
  `-DHALCYON_ENABLE_GOOGLETEST=ON`.

This first version deliberately keeps the backend bridge small: the graph owns
semantic ordering and lifetime decisions, while Vulkan retains explicit image
barriers and command recording. Optional Tracy CPU instrumentation, binding of
the bindless set into the demo pipeline, and generic per-pass GPU timestamp
routing are available without making them mandatory for the base renderer.
Advanced GPU-driven rendering, the Visibility Buffer, and ray-tracing
extensions remain assigned to later milestones. M2 infrastructure is enabled by default; pass
`-DHALCYON_BUILD_EXPERIMENTAL_M2=OFF` to build only the M0/M1 baseline.

## Language Policy

All project-owned source code, comments, logs, tests, shaders, build and
configuration files, filenames, and documentation must be written in English.
Third-party dependencies and generated output are excluded from this policy.

## Build

Requirements: Windows 11, Visual Studio 2022 C++ Build Tools with MSVC v143,
CMake 3.28 or newer, Ninja, and Vulkan SDK 1.3 or newer. Run the following from
a Developer PowerShell or Command Prompt, or from CLion with an initialized
MSVC environment:

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
```

Use the RelWithDebInfo preset for performance measurements:

```powershell
cmake --preset windows-msvc-relwithdebinfo
cmake --build --preset windows-msvc-relwithdebinfo
ctest --preset windows-msvc-relwithdebinfo
```

If DXC is unavailable, configuration emits a warning, but the embedded M1
shaders remain usable. Install the Vulkan SDK and configure again to enable the
HLSL validation target.

## Run the Sandbox

```powershell
out\build\windows-msvc-debug\Halcyon.exe --frames 300
```

Available options are `--width N`, `--height N`, `--frames N`,
`--no-validation`, and `--help`. The render loop pauses image acquisition while
the window is minimized and recreates the swapchain after restoration.

## Standalone Examples

The repository also builds independent examples with isolated output
directories.  See [Examples/README.md](./Examples/README.md) for the available
targets, run commands, and the procedure for adding a new example without
replacing previous binaries.

## Repository Layout

```text
Source/Core                 Backend-neutral results, logging, and stable handles
Source/Renderer/Scene       Camera and FramePacket data contracts
Source/Renderer/Graph       M2 RenderGraph and barrier planning
Source/Renderer/Resources   Upload/deletion and bindless slot infrastructure
Source/Renderer/Vulkan      Vulkan 1.3 backend
Source/Engine                Backend-neutral Engine and View orchestration
Source/Application           Window, input, lifecycle, and diagnostics layer
Source/Sandbox              Runnable demonstration application
Examples                    Independent executable examples
Source/Cooker               Deterministic resource-manifest tool
Tests                       CPU unit tests
```

Each milestone should retain a RenderDoc capture, screenshots, and a performance
CSV. Performance results are comparable only when hardware, driver, resolution,
scene, and quality settings are identical.
