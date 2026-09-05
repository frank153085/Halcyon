# M3 Architecture Notes

This note records the decisions and known limitations of the M3 traditional
rendering baseline. It is an artifact of the M3 milestone and should be read
before changing the frame submission path for M4.

## Key design decisions

- **Four-level CSM fixed topology.** Cascaded shadow maps use four stable
  splits and a 3x3 PCF kernel. The split layout is deterministic so captures
  and golden images are reproducible.
- **Reversed-Z convention.** Depth uses `D32_SFLOAT`, clear value `0.0`, and
  `GREATER_OR_EQUAL` comparisons. Farther geometry has smaller depth values.
- **SceneManager/Vulkan boundary.** `SceneManager` owns ECS state and the
  persistent `SceneDatabase`; Vulkan receives remapped, backend-neutral frame
  packets and resource records. ECS policy does not leak into the renderer.
- **FrameGraph resource lifetime.** Transient attachments (G-buffer, depth,
  lighting and post-process intermediates) are allocated per compiled graph
  and may alias. Persistent resources (scene buffers, uploaded meshes/materials,
  swapchain-independent history) are owned by the renderer and survive graph
  recompilation.

## Known limitations carried into M4

- IBL environment lighting currently comes from the procedural `proceduralSky()`
  path in `Source/Renderer/Vulkan/HalcyonVulkanRenderer.cpp`; there is no HDRI
  disk-loading path yet.
- Forward transparency uses the single-light simplified PBR path in
  `Shaders/forward_transparent.frag.hlsl`. It does not share clustered-light or
  IBL evaluation with the opaque deferred path.
- CSM uses 3x3 PCF without cascade interval blending (`Shaders/pbr.frag.hlsl`).
- TAA resolve (`Shaders/taa.comp.hlsl`) has no camera Halton/2x2 jitter; it
  relies on history clamping for temporal stability.
- Golden images are generated under `out/`, which is ignored by Git. A checked-
  in baseline and automated image-regression job are therefore still required.

## Capture and regression workflow

1. Build the debug or RelWithDebInfo preset and fetch M3 assets.
2. Run `HalcyonM3Demo.exe` with deterministic scene, frame count, screenshot,
   and `--perf-csv` arguments (see the README examples).
3. For GPU inspection, launch with `--no-validation` and capture a frame in
   RenderDoc. Inspect the CSM, G-buffer, clustered deferred lighting, TAA, and
   ACES tonemap markers.
4. Compare a screenshot with `HalcyonGoldenCompare.exe --actual ... --golden ...`.
   The M3 gate uses SSIM >= 0.995.
5. Keep the screenshot, RenderDoc capture metadata, and performance CSV
   together when publishing a milestone artifact.
