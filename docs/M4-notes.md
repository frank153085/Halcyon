# M4 GPU-Driven Foundation Notes

M4 keeps the M3 `DeferredIndexed` submission path intact and introduces a
parallel GPU-driven path. The backend-neutral scene contract is defined in
`Source/Renderer/Scene/GpuScene.h`.

## GPU Scene layout

Instances use stable slots. Transform, bounds, and mesh/material metadata are
stored as separate arrays (SoA), allowing transform-only updates without
rewriting metadata. Bounds are world-space AABBs plus a conservative bounding
sphere and are recomputed only when a transform changes. Slot reclamation is
deferred until the submitted timeline has completed.

## Compute stages

`frustum_cull.comp.hlsl` appends visible slot indices using an atomic counter;
`build_indirect_commands.comp.hlsl` emits one indexed indirect command per
visible slot (mesh batching is intentionally left for a later optimization);
`hiz_build.comp.hlsl` performs reversed-Z 2x2 minimum reduction. The latter is
the conservative choice: a maximum reduction could incorrectly classify a
partially covered object as fully occluded.

The two-phase occlusion scheduler is staged after the culling/indirect path.
Phase 1 consumes the previous frame's Hi-Z pyramid, while phase 2 re-tests
potential false occlusions against the current frame depth before submitting
the G-buffer pass.

## Known performance scope

The initial indirect builder emits one command per instance. It is functionally
correct and provides the required synchronization/descriptor boundaries, but
does not yet batch commands by mesh. Mesh batching and full two-phase GPU
integration are follow-up optimizations.
