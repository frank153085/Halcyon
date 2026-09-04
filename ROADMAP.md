# Halcyon Roadmap

> Positioning: A GPU-driven, reproducible modern real-time renderer for individual study

This document defines Halcyon's implementation path and milestone acceptance criteria. The project retains a traditional deferred path as its correctness baseline, then incrementally adds GPU-driven rendering, virtualized geometry, dynamic global illumination, virtual shadows, and optional neural-rendering research.

## Contents

- [Project Goals](#project-goals)
- [Architecture Plan](#architecture-plan)
- [Capability and Fallback Matrix](#capability-and-fallback-matrix)
- [Per-Frame Data Flow](#per-frame-data-flow)
- [Milestones](#milestones)
- [Testing and Performance Acceptance](#testing-and-performance-acceptance)
- [Milestone Artifacts](#milestone-artifacts)
- [Explicit Boundaries](#explicit-boundaries)

## Project Goals

- Use C++20, Vulkan 1.3, and HLSL/DXC to study modern real-time rendering without expanding Halcyon into a complete game engine.
- Generate visibility lists and draw parameters on the GPU to reduce CPU draw-call and resource-binding overhead as scene complexity grows.
- Retain both traditional deferred and advanced GPU-driven paths for reproducible image-quality, correctness, and performance A/B comparisons.
- Use a frame-budget controller to coordinate resolution, geometric error, shadow, GI, and ray-tracing quality, with P95 and P99 as primary performance metrics.
- Fix scenes, cameras, time steps, and random seeds to support capture/replay, image comparison, performance CSV files, and quality-decision logs.

## Architecture Plan

### Layered Rendering Paths

1. `DeferredIndexed`: a compact traditional G-buffer path retained permanently as the correctness and compatibility baseline.
2. `GpuDrivenIndexed`: Compute Culling generates visible instances and `VkDrawIndexedIndirectCommand` entries consumed by `vkCmdDrawIndexedIndirectCount`.
3. `VirtualGeometryIndexed`: meshlets and the LOD DAG continue to use Indexed Indirect, providing the required compatibility path for virtualized geometry.
4. `VirtualGeometryMeshShader`: when `VK_EXT_mesh_shader` is available, the same visible-meshlet list is converted into `VkDrawMeshTasksIndirectCommandEXT` entries.
5. `DynamicGI`: SDF and screen-space techniques provide the base path, Ray Query improves near-field accuracy, and VSM uses a software page table plus a physical atlas without requiring Vulkan sparse residency.

### Compute Culling and Mesh Shaders

These technologies do not conflict. Compute Culling answers which instances or meshlets are visible. Indexed Indirect or Mesh Shaders determine how those visible elements are executed.

```text
Compute Culling
  -> VisibleInstanceList / VisibleMeshletList
      -> Indexed Command Builder -> vkCmdDrawIndexedIndirectCount
      -> Mesh Task Command Builder -> vkCmdDrawMeshTasksIndirectCountEXT
```

The implementation order is fixed: complete Compute Culling plus Indexed Indirect first, then add the Mesh Shader path. Compute passes perform coarse instance, LOD, and occlusion culling. Mesh and Task Shaders perform only fine-grained meshlet work. This design shares the GPU Scene, Hi-Z pyramid, Bindless table, and visibility lists while retaining a debuggable baseline and a fallback for devices without Mesh Shader support.

### Scene and Resource Organization

- Static meshes, materials, textures, and initial instances enter a persistent `SceneDatabase` and upload only when created or changed.
- `SceneManager` owns the SceneDatabase, scene ECS, asset/instance handles, and
  backend synchronization. All runnable programs use its file-backed or
  procedural scene configuration; render backends never parse application
  asset files directly.
- `FramePacket` remains immutable and contains no Vulkan handles. It carries only camera data, lights, and dirty rigid-instance updates.
- The first version uses a thin scene ECS façade (generation-checked entities,
  dense component stores, and a render extractor). A scheduler, reflection
  system, and gameplay-oriented ECS are intentionally deferred so this layer
  can be replaced without changing renderer-facing packet contracts.
- The CPU uses stable generation handles. The upload stage resolves them into dense 32-bit GPU Scene indices.
- The GPU Scene uses SoA buffers for transforms, bounds, meshes, materials, LOD state, and flags.
- `HalcyonCooker` produces deterministic caches with schema versions and content hashes, progressively adding glTF data, fixed LODs, meshlets, the cluster DAG, mesh SDFs, and Surface Cache proxy data.
- Every FrameGraph pass declares resource reads, writes, queue class, and access type. The first implementation executes on the graphics queue; asynchronous compute is enabled only after correctness is stable and timestamp data demonstrates a benefit.

### Reversed-Z Hi-Z Convention

Halcyon uses D32 depth, clears depth to zero, and tests with `GREATER_OR_EQUAL`. A Hi-Z pyramid that stores reversed-Z depth directly uses a **2x2 minimum reduction** at each level. This conservatively retains the farthest occluder over the covered region.

A maximum reduction could incorrectly treat a nearby surface that covers only a few pixels as the occluder for the entire region, causing false culling. If the representation later changes to linear view-space depth, the reduction operator, comparison direction, and automated tests must change together.

## Capability and Fallback Matrix

| Feature | Default or Required Path | Optional Enhancement | Fallback |
|---|---|---|---|
| Base rendering | Vulkan 1.3, Dynamic Rendering, Synchronization2, Timeline Semaphore | None | Fail startup with a capability report |
| Geometry submission | Compute Culling + Indexed Indirect Count | Mesh Shader Indirect | Deferred Indexed |
| Resource binding | Descriptor Indexing Bindless | Larger capability-clamped resource tables | Default resources and traditional binding |
| Occlusion culling | Reversed-Z Hi-Z and Two-Phase Occlusion | Asynchronous compute | Frustum and LOD culling only |
| Visibility shading | Visibility Buffer + Compute Shading | Fragment Barycentric | Compact G-buffer |
| Virtualized geometry | Meshlets + Indexed Indirect | LOD DAG and Mesh Shaders | Fixed-LOD conventional meshes |
| Micro-triangles | Hardware rasterization | Experimental compute rasterization | Continue with hardware rasterization |
| Global illumination | SSGI/SDF tracing + Radiance Cache | Near-field Ray Query | IBL + screen-space approximation |
| Shadows | CSM | Virtual Shadow Maps | Retain CSM |
| Upscaling and caches | Non-neural TAAU and Surface Cache | Experimental neural radiance cache | Non-neural path |

The capability tier is selected at startup and the renderer does not switch backend paths at runtime. The frame-budget controller adjusts only quality controls within the selected capability set.

## Per-Frame Data Flow

The FrameGraph prunes passes that are unavailable at the current milestone or capability tier. The target data flow is:

```text
Wait for the current frame timeline
  -> Collect deferred resources and completed Bindless slots
  -> Upload budgeted resources and dirty GPU Scene data
  -> Phase 1: frustum / LOD / previous-frame Hi-Z culling
  -> Generate Indexed or Mesh Task indirect commands
  -> Render first-phase depth and visibility
  -> Build the current Hi-Z pyramid
  -> Phase 2: retest and render potential false occlusion results
  -> Build the final Hi-Z pyramid for the next frame
  -> Request, allocate, and update VSM pages
  -> Update SDF / Ray Query GI and the Radiance Cache incrementally
  -> Classify visibility by material and run Compute Shading
  -> Render forward transparency
  -> TAAU -> ACES Tonemap -> ImGui
  -> Present
  -> Read delayed timestamps and update the frame-budget controller
```

## Milestones

### Progress Overview

- [x] M0 Engineering Foundation
- [x] M1 Vulkan Vertical Slice
- [x] M2 Core Infrastructure - first version complete; advanced integration remains deferred
- [x] M3 Traditional Quality Baseline (runnable baseline complete)
- [ ] M4 GPU-Driven Foundation
- [ ] M5 Virtualized Geometry V1
- [ ] M6 Virtualized Geometry V2
- [ ] M7 Dynamic GI and VSM
- [ ] M8 Frame-Budget Control and Graduation Experiments
- [ ] M9 Neural-Rendering Research - optional and non-blocking

### M0 Engineering Foundation (Complete)

**Dependencies:** None.

**Core deliverables:** CMake 3.28, MSVC v143 and Ninja presets, separate `HalcyonCore`, `HalcyonRenderer`, `HalcyonSandbox`, and `HalcyonCooker` targets, unified result and error paths, logging, generation handles, and a Validation Messenger.

**Acceptance gate:** Debug and RelWithDebInfo builds are reproducible. Initialization failures, window exit, and normal shutdown produce no validation errors.

### M1 Vulkan Vertical Slice (Complete)

**Dependencies:** M0.

**Core deliverables:** Vulkan 1.3 device and queue selection, swapchain, three frame contexts, Timeline Semaphores, Synchronization2, Dynamic Rendering, D32 reversed-Z, resize/minimize/out-of-date handling, GPU timestamps, and a runnable triangle slice.

**Acceptance gate:** The Sandbox runs for 300 frames and exits normally. Resize, minimize, and restoration do not crash. Debug validation reports no warnings or errors, and RenderDoc can capture a complete frame.

### M2 Core Infrastructure (First Version)

**Dependencies:** M1.

**Core deliverables:** VMA, GPU resource pools, timeline-based deferred deletion, a Vulkan Bindless Descriptor Table companion, FrameGraph compilation and CPU execution, Barrier2 planning, HLSL compilation/reflection/hot reload, an optional ImGui diagnostics overlay, and the initial GPU timestamp hooks. The implementation enables CPU FrameGraph and Bindless infrastructure by default, adds callback execution and cycle-safe diagnostics, provides capability-clamped descriptor tables with typed image/buffer writes and frame-timeline collection, validates SPIR-V modules, provides lightweight reflection metadata, and provides transactional shader/pipeline replacement. The scene pass is timed in the demo and optional GoogleTest coverage is available.

**Acceptance gate:** FrameGraph topology, cycle detection, culling, lifetime, barrier, and callback execution tests pass. No live resource allocations remain after shutdown. Bindless slots are not reused before their timeline completes. Invalid shader binaries are rejected, and failed shader or pipeline replacement leaves the last valid object active. The Vulkan bridge executes the compiled scene pass, binds the optional bindless set, routes timestamps for every compiled pass, and keeps Tracy instrumentation optional. The first version is complete after these CPU and local Vulkan checks; explicit backend barrier translation and shader-side bindless indexing remain deliberately small follow-up tasks.

### M3 Traditional Quality Baseline (Complete)

**Dependencies:** M2.

**Core deliverables:** fastgltf static scenes, metallic-roughness PBR, IBL, clustered lighting, CSM, a compact G-buffer, forward transparency, HDR/ACES, motion vectors, and TAA.

**Acceptance gate:** Damaged Helmet and Sponza render correctly. A fixed camera, exposure, and time step produce stable golden images. The traditional path becomes the correctness and performance baseline for later paths.

The checked-in baseline includes deterministic scene download/manifest tooling,
fastgltf validation, rigid node/material extraction, generated defaults,
per-primitive material draw ranges, screenshots, Golden SSIM comparison, and
performance CSV output. Vulkan executes the full production graph with four CSM
depth scopes, G-buffer MRT, GPU cluster build and overflow readback, deferred
PBR/IBL, forward transparency, TAA compute, ACES tonemap, and present/readback.
FrameGraph transient and persistent resources are materialized through VMA and
recreated on resize. CPU clustered-lighting and single-scope forward paths are
not part of the M3 backend. Known device requirement: Vulkan 1.3 with dynamic
rendering, synchronization2, storage-buffer atomics, required image formats, and
a host-visible readback path. Captures and CSVs are written under `out/build/`
by the commands in `README.md`.

### M4 GPU-Driven Foundation (Planned)

**Dependencies:** M2 plus the renderable scenes and baseline materials from M3.

**Core deliverables:** GPU Scene SoA data, per-instance bounding spheres and AABBs, Compute Frustum Culling, GPU counters, an Indirect Command Builder, `vkCmdDrawIndexedIndirectCount`, a reversed-Z Hi-Z pyramid, and Two-Phase Occlusion.

**Acceptance gate:** CPU visibility work remains below 1 ms in a stress scene containing 100,000 independent instances. No per-material descriptor-set switches occur. Scripted-camera results contain no truly visible object missing relative to the reference path with occlusion culling disabled.

### M5 Virtualized Geometry V1 (Planned)

**Dependencies:** M4.

**Core deliverables:** The Cooker uses meshoptimizer to generate fixed LODs and meshlets with default limits of 64 vertices and 124 triangles. It stores bounding spheres, normal cones, and geometric error. Runtime work includes meshlet culling, Indexed Indirect submission, the Visibility Buffer, material classification, and Compute Shading.

**Acceptance gate:** A source model with at least ten million triangles imports without handcrafted LODs. `DeferredIndexed`, `GpuDrivenIndexed`, and `VirtualGeometryIndexed` can render the same scene. Image differences stay within established FLIP/SSIM thresholds, with no out-of-bounds access or visible cracks.

### M6 Virtualized Geometry V2 (Planned)

**Dependencies:** M5.

**Core deliverables:** METIS cluster groups, boundary-locked simplification, the LOD DAG, screen-space error selection, and the Mesh Shader indirect path. Compute rasterization for micro-triangles remains an isolated experiment and does not enter the compatibility path.

**Acceptance gate:** The DAG is acyclic, parent errors are monotonic, and all references are valid. LOD transitions exhibit no cracks or persistent oscillation. Indexed and Mesh Shader paths produce consistent results for the same visible set. Devices without Mesh Shader support fall back automatically.

### M7 Dynamic GI and VSM (Planned)

**Dependencies:** M5. The M6 DAG and Mesh Shader path are not hard dependencies.

**Core deliverables:** Offline mesh SDFs, a camera-relative Global SDF Clipmap, sphere tracing, incremental Radiance/Surface Cache updates, optional near-field Ray Query, and VSM backed by a software page table and physical atlas.

**Acceptance gate:** Indirect light and shadows respond to moving rigid objects or lighting changes without baking. Cache-update budgets are queryable. The scene remains complete with RT disabled or on a GPU without RT support. Missing, reclaimed, or over-budget VSM pages use a safe fallback.

### M8 Frame-Budget Control and Graduation Experiments (Planned)

**Dependencies:** Stable quality controls and timestamps from M4 through M7.

**Core deliverables:** Coordinated control of internal resolution, LOD and geometric screen error, CSM/VSM, GI ray count, cache-update ratio, and Ray Query. Add FramePacket capture/replay, automated cameras, quality-decision logs, and A/B performance reports.

**Acceptance gate:** After eight consecutive over-budget frames, lower only one quality level. After 120 consecutive frames below 80% of the budget, raise only one quality level. Apply a 60-frame cooldown after each adjustment. Recover from a load spike within two seconds without persistent oscillation under stable load. Compared with fixed maximum quality, reduce over-budget frames by at least 50%, and keep steady-state P95 GPU frame time within 1.2 times the target.

### M9 Neural-Rendering Research (Optional)

**Dependencies:** The non-neural cache from M7 and the reproducible experiment framework from M8.

**Core deliverables:** Keep non-neural TAAU and the Surface Cache as baselines, then independently experiment with a small neural radiance cache or cache reconstruction. Keep training tools separate from runtime inference and disable the feature by default.

**Acceptance gate:** Missing models, loading failures, or unsuitable hardware automatically use the non-neural path. The feature may enter an optional build only after same-machine A/B testing demonstrates image-quality benefit, stability, and acceptable cost. General-purpose neural upscaling, frame generation, and a complete neural-material system remain out of scope.

## Testing and Performance Acceptance

### CPU Unit Tests

- Generation handles, default resource slots, deferred deletion, and timeline-based slot recycling.
- FrameGraph topology, cycle detection, pass culling, resource lifetimes, and barrier-state mapping.
- Shader reflection layouts, 16-byte C++/HLSL alignment, and cache-version compatibility.
- Meshlet boundaries, LOD error, cluster-DAG acyclicity, reference ranges, and Cooker determinism.
- Frame-budget thresholds, cooldowns, upgrade/downgrade selection, and decision logs.

### Vulkan Integration Tests

- Swapchain out-of-date handling, minimize, resize, Alt-Tab, and frequent resource creation and destruction.
- Delayed Bindless-slot reuse, Indirect Count bounds, counter clearing, and command-buffer barriers.
- Reversed-Z Hi-Z reduction and occlusion comparisons, Two-Phase recovery draws, and rapid camera motion.
- Capability fallback combinations for Indexed/Mesh Shader, Visibility/G-buffer, Ray Query/SDF, and VSM/CSM.
- On device loss, report the device, driver, last pass, and resource statistics before a safe exit.

### Image and Performance Tests

- Fix random seeds, camera, exposure, and time step. Compare with SSIM/FLIP without requiring pixel-identical results across drivers.
- In RelWithDebInfo, warm up for 300 frames and measure 1,800 frames. Record CPU/GPU P50, P95, P99, per-pass times, memory, visible instance/meshlet counts, and indirect-command counts.
- Compare results only on identical hardware, driver, resolution, scene, and quality settings. Mark a same-machine regression above 5% as failed and attach a per-pass timing delta report.
- Every advanced path must produce same-trajectory image and performance A/B reports against the deferred baseline.

## Milestone Artifacts

Every completed milestone must retain:

- A short architecture note documenting key decisions and known limitations.
- A runnable demo and deterministic test scene.
- A complete replayable RenderDoc capture.
- Fixed-view screenshots and image-comparison results.
- A performance CSV with a description of the test environment.

A milestone is complete only after its functionality, fallbacks, tests, and artifacts all pass. Prototype code alone does not complete a milestone.

## Explicit Boundaries

- The primary platform is Windows 11 with Vulkan. Halcyon does not build a cross-API RHI; D3D12, Linux, and Metal remain independent extensions.
- Only static and rigid scenes are in scope. Skeletal animation, morph targets, hair, and complex transparent materials are excluded.
- Editors, a full gameplay ECS, physics, audio, scripting, networking, and a complete game framework are excluded. The small scene ECS façade used by the first version is limited to render extraction.
- Feature-complete, commercial-engine-scale virtualized geometry and dynamic global illumination are out of scope; Halcyon implements only the constrained learning paths defined above. General virtual texturing, path tracing, ReSTIR GI, and neural frame generation are excluded.
- Mesh Shaders, compute rasterization, Ray Query, and neural modules must never become hard runtime requirements.
- Performance conclusions apply only to documented test environments and do not claim that an individual algorithm outperforms a complete commercial engine.
- All project-owned code, comments, configuration, filenames, and documentation use English.
