# Halcyon

[Project Roadmap](./ROADMAP.md)

Halcyon is a real-time renderer built for individual study. The repository
now provides the first, intentionally small M2 infrastructure increment and a
runnable M3 traditional-quality scene demonstration. The
M0/M1 Vulkan vertical slice remains the correctness baseline while render
graph, bindless, shader, and profiling foundations stay independently usable.

## Current Milestone: M3 traditional quality baseline

- Reproducible Debug and RelWithDebInfo presets for MSVC v143 and Ninja.
- Separate `HalcyonCore`, `HalcyonRenderer`, `HalcyonEngine`,
  `HalcyonApplication`, `HalcyonSandbox`, and `HalcyonCooker` targets.
- Vulkan 1.3 instance and device selection, Validation Messenger, Dynamic
  Rendering, Synchronization2, and Timeline Semaphores.
- Three frame contexts, swapchain recreation, and safe minimize, resize, and
  out-of-date handling.
- A reversed-Z camera convention with D32 depth and `GREATER_OR_EQUAL`.
- Build-time HLSL-to-SPIR-V compilation through DXC. All M3 pipelines require
  generated SPIR-V at runtime; missing shader binaries fail initialization.
- CPU-side tests for generation handles, deferred deletion, and the upload
  ring foundation.
- CPU render-graph compilation/execution, barrier planning, bindless slot
  lifetime tracking, and frame-budget infrastructure.
- A deliberately thin first-version scene ECS: generation-checked entities,
  dense transform/renderable/light component stores, and extraction into an
  immutable `FramePacket`. The renderer remains independent of ECS policy.
- A backend-neutral `SceneManager` that owns SceneDatabase assets and scene
  instances. File-backed glTF/GLB scenes and in-memory procedural scenes use
  the same transactional registration, instancing, and GPU upload path.
- A Vulkan bindless descriptor-set companion that maps typed CPU slots to
  sampled/storage image, sampler, uniform-buffer, and storage-buffer arrays.
- Renderer-owned bindless slot collection is synchronized with the frame
  timeline when descriptor indexing is available.
- Shader module validation with an optional SPIR-V Tools pass, lightweight
  descriptor reflection, and a reload-safe replacement API for development
  builds.
- Static glTF/GLB scene loading through the pinned fastgltf reader, with
  generated normals/tangents, rigid node transforms, metallic-roughness
  material metadata, and a persistent scene database.
- Backend-neutral PBR/IBL evaluation, logarithmic clustered-light assignment,
  cascaded shadow splits, compact octahedral G-buffer packing, forward
  transparency helpers, HDR/ACES tonemapping, motion vectors, and temporal AA
  resolve logic. Matching HLSL passes are provided for DXC builds.
- Optional Dear ImGui diagnostics overlay; enable it with
  `-DHALCYON_ENABLE_IMGUI=ON`.
- Optional GoogleTest coverage; enable it with
  `-DHALCYON_ENABLE_GOOGLETEST=ON`.

The Vulkan backend executes the complete multi-pass M3 graph: four CSM depth
passes, G-buffer MRT, GPU cluster build, deferred PBR/IBL, forward transparency,
TAA compute, ACES tonemap, and present/readback. Each pass has its own dynamic
rendering scope and descriptor layout, while the FrameGraph provider owns VMA
materialization and transient/persistent lifetimes. Device, format, descriptor,
and shader requirements are checked during initialization; unsupported devices
fail with a diagnostic instead of selecting a reduced path.
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

When CMake is launched outside a Visual Studio developer shell, use the
reproducible helper. It imports `VsDevCmd`, detects a stale MinGW `ld.exe`
entry in `CMakeCache.txt`, and configures an isolated build directory:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/configure_m3_msvc.ps1 `
  -BuildDir out/build/m3-msvc-debug -Build -FetchAssets
```

The large M3 sample assets are intentionally ignored by Git. The equivalent explicit download is
`cmake --build out/build/m3-msvc-debug --target HalcyonFetchM3Assets`.

Use the RelWithDebInfo preset for performance measurements:

```powershell
cmake --preset windows-msvc-relwithdebinfo
cmake --build --preset windows-msvc-relwithdebinfo
ctest --preset windows-msvc-relwithdebinfo
```

DXC is mandatory for the M3 renderer: configuration fails with a diagnostic if
it cannot be found. When the Vulkan SDK also provides `spirv-val`, CMake adds a
`HalcyonShaderValidation` target and validates every generated module before
the renderer is linked. Install the Vulkan SDK and configure again after fixing
the toolchain path.

## Run the Sandbox

```powershell
out\build\windows-msvc-debug\Halcyon.exe --frames 300
```

Available options are `--width N`, `--height N`, `--frames N`,
`--no-validation`, and `--help`. The render loop pauses image acquisition while
the window is minimized and recreates the swapchain after restoration.
The Sandbox and standalone textured example both load the checked-in Monkey
glTF through `SceneManager`; the Vulkan backend no longer contains an OBJ or
startup-texture loader.

## Run the M3 demo

`HalcyonM3Demo` uses fixed cameras and a fixed 60 Hz timestep by default. The
downloaded Damaged Helmet and Sponza assets are selected with `--scene`:

```powershell
out\build\m3-msvc-debug\HalcyonM3Demo.exe `
  --scene damaged-helmet --frames 300 --width 1280 --height 720 `
  --screenshot out\captures\helmet.png `
  --perf-csv out\captures\helmet.csv --no-validation

out\build\m3-msvc-debug\HalcyonM3Demo.exe `
  --scene sponza --frames 300 --screenshot out\captures\sponza.png `
  --perf-csv out\captures\sponza.csv --no-validation
```

The deterministic stress scene exercises large instance counts without
downloading additional assets:

```powershell
out\build\m3-msvc-debug\HalcyonM3Demo.exe `
  --scene stress --instance-count 100000 --frames 1 --no-validation
```

For a GPU-driven versus traditional CPU-path stress comparison, run
`scripts\\run_m4_stress.bat`. It writes `out\\captures\\m4-stress.csv` for
the GPU-driven two-phase run and `out\\captures\\m4-stress-legacy.csv` for
the CPU baseline. The `--no-gpu-driven` switch selects the traditional path.

On a machine with the M3 assets and a Vulkan device, the two-scene regression
wrapper is `powershell -ExecutionPolicy Bypass -File scripts/run_regression.ps1`.
It writes screenshots and performance CSV files under `out/captures/regression`
and fails on a non-zero demo or golden-image comparison result.
The M4 submission-path A/B gate is
`powershell -ExecutionPolicy Bypass -File scripts/run_m4_ab.ps1`; it renders
both scenes with fixed timestep and TAA disabled, then compares the
GPU-driven capture against the legacy baseline at SSIM 0.995.

Quality switches are `--fixed-dt <seconds>` (for example, `--fixed-dt 0.016666`), `--exposure`, `--no-taa`,
`--no-clustered-lighting`, `--no-transparency`, and the opt-in
`--gpu-driven` path. `--two-phase-occlusion` enables GPU-driven rendering plus
the previous/current-frame Hi-Z re-test. A standalone image gate is
available for CI and RenderDoc captures:

GPU-driven runs also perform an asynchronous per-frame visibility audit. The
performance CSV exposes `gpu_visibility_missing_count` (CPU-reference slots
absent from the GPU result) and `gpu_visibility_validation_passed`; the audit
consumes a completed frame-slot readback without blocking command recording.
For a deterministic stress-scene orbit and an automated pass/fail check, run
`powershell -ExecutionPolicy Bypass -File scripts/run_m4_visibility.ps1`.
The literal R32Uint attachment set comparison (frustum-only reference versus
two-phase occlusion) is available through
`powershell -ExecutionPolicy Bypass -File scripts/run_m4_instance_id.ps1`.

```powershell
out\build\m3-msvc-debug\HalcyonGoldenCompare.exe `
  --actual out\captures\helmet.png --golden path\to\helmet-golden.png
```

When `--golden` is supplied without an explicit `--frames`, the runner uses a
640×360 capture with 120 deterministic warm-up frames before comparing the
final image (SSIM threshold 0.995). Explicit frame counts remain available for
fast smoke tests and self-comparisons.

The demo submits stable SceneManager instances through the complete Vulkan M3
graph. Damaged Helmet and Sponza resolve their external or embedded textures
through the same material upload path, with deterministic default textures for
missing material channels. Cluster ranges/indices and overflow counters are
written by the GPU compute pass and consumed by deferred lighting; no CPU
clustered-lighting or forward-only compatibility path is used.

For a RenderDoc capture, launch the demo with `--frames 300 --no-validation`,
press **Capture Frame** after the window appears, and inspect the `G-buffer`,
`Clustered deferred lighting`, `TAA resolve`, and `ACES tonemap` markers in
the event browser. The CPU reference tests and `HalcyonGoldenCompare` remain
usable on machines without a Vulkan device.

## Standalone Examples

The repository also builds independent examples with isolated output
directories.  See [Examples/README.md](./Examples/README.md) for the available
targets, run commands, and the procedure for adding a new example without
replacing previous binaries.

## Repository Layout

```text
Source/Core                 Backend-neutral results, logging, and stable handles
Source/Renderer/Scene       Camera and FramePacket data contracts
Source/Renderer/Graph       M2 FrameGraph and barrier planning
Source/Renderer/Resources   Upload/deletion and bindless slot infrastructure
Source/Renderer/Vulkan      Vulkan 1.3 backend
Source/Engine                Engine, View, and SceneManager orchestration
Source/Application           Window, input, lifecycle, and diagnostics layer
Source/Sandbox              Runnable demonstration application
Examples                    Independent executable examples
Source/Cooker               Deterministic resource-manifest tool
Tests                       CPU unit tests
```

Each milestone should retain a RenderDoc capture, screenshots, and a performance
CSV. Performance results are comparable only when hardware, driver, resolution,
scene, and quality settings are identical.
