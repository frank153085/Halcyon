# Halcyon

[Project Roadmap](./ROADMAP.md)

Halcyon is a real-time renderer built for individual study. The repository is
currently at the M0/M1 vertical-slice milestone: lifecycle management,
synchronization, and error paths are made reliable before more advanced
rendering algorithms are introduced.

## Current Milestone: M0/M1

- Reproducible Debug and RelWithDebInfo presets for MSVC v143 and Ninja.
- Separate `HalcyonCore`, `HalcyonRenderer`, `HalcyonSandbox`, and
  `HalcyonCooker` targets.
- Vulkan 1.3 instance and device selection, Validation Messenger, Dynamic
  Rendering, Synchronization2, and Timeline Semaphores.
- Three frame contexts, swapchain recreation, and safe minimize, resize, and
  out-of-date handling.
- A reversed-Z camera convention with D32 depth and `GREATER_OR_EQUAL`.
- Build-time HLSL-to-SPIR-V validation through DXC, with embedded triangle
  shaders retained as an offline fallback.
- CPU-side tests for generation handles, deferred deletion, and the upload
  ring foundation.

Advanced GPU-driven rendering, the Visibility Buffer, ray-tracing extensions,
and the frame-budget controller are assigned to later milestones from M4
through M8. Existing
RenderGraph, Bindless, and frame-budget prototypes are excluded from the
default M0/M1 build. Enable them for isolated study with
`-DHALCYON_BUILD_EXPERIMENTAL_M2=ON`.

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

## Repository Layout

```text
Source/Core                 Backend-neutral results, logging, and stable handles
Source/Renderer/Scene       Camera and FramePacket data contracts
Source/Renderer/Graph       M2 RenderGraph prototype, disabled by default
Source/Renderer/Resources   M1 upload/deletion and M2 Bindless prototypes
Source/Renderer/Vulkan      Vulkan 1.3 backend
Source/Sandbox              Runnable demonstration application
Source/Cooker               Deterministic resource-manifest tool
Tests                       CPU unit tests
```

Each milestone should retain a RenderDoc capture, screenshots, and a performance
CSV. Performance results are comparable only when hardware, driver, resolution,
scene, and quality settings are identical.
