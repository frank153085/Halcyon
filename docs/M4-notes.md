# M4 GPU-Driven Foundation Notes

M4 keeps the M3 `DeferredIndexed` submission path intact and introduces a
parallel GPU-driven path. The backend-neutral scene contract is defined in
`Source/Renderer/Scene/GpuScene.h`.

## GPU Scene layout

Instances use stable slots. Transform, bounds, and mesh/material metadata are
stored as separate arrays (SoA), allowing transform-only updates without
rewriting metadata. Bounds are world-space AABBs plus a conservative bounding
sphere and are recomputed only when a transform changes. Slot reclamation is
deferred until the submitted frame timeline has completed; the Vulkan frame
loop feeds that same completion value to both the slot allocator and bindless
descriptor table.

`MaterialGpuData` is the row ABI for the bindless material table (factors plus
five texture indices). GPU-driven draws use one global bindless descriptor set
with a 256-entry sampled-image array and a 16-entry sampler array when device
capacities permit it; devices below that fixed shader ABI use the M3
material-set fallback. The unchanged deferred path remains available.

## Compute stages

`frustum_cull.comp.hlsl` appends visible slot indices using an atomic counter;
it also reads the per-slot `MeshMaterialRow.lodState` and writes a parallel
reserved state stream for the next LOD stage;
`build_indirect_commands.comp.hlsl` first links visible slots into per-mesh
lists, then emits one indexed indirect command per mesh with an instanced
`instanceCount`. A compacted slot list is consumed by the vertex shader via
`firstInstance`, so instances sharing one mesh no longer create one command
each. Mesh vertex/index data is consolidated into renderer-owned streams when
scene assets change, and a GPU mesh-draw table supplies each command's
`indexCount`, `firstIndex`, and `vertexOffset`. This allows mixed opaque meshes
and bindless materials to share one indirect command list;
transparent or double-sided packets retain the M3 CPU path.
`hiz_build.comp.hlsl` performs reversed-Z 2x2 minimum reduction. Hi-Z mip 0 is
half the scene-depth resolution, so its first dispatch is the first reduction
rather than a same-size copy. Minimum reduction is the conservative choice: a
maximum reduction could incorrectly classify a partially covered object as
fully occluded.

The Hi-Z pyramid is a persistent FrameGraph resource and is built level by
level after the G-buffer pass. When enabled, phase 1 classifies the frustum
candidate list against the previous pyramid, while phase 2 re-tests rejected
objects against the current pyramid and overlays any newly visible instances
into the existing G-buffer. The feature is opt-in (`enableTwoPhaseOcclusion`)
and the first frame conservatively skips history-based rejection. Because the
overlay can change depth, the renderer rebuilds the complete final pyramid
after phase 2; only that pyramid is retained for the following frame. The Hi-Z
pass declares its LOAD/STORE G-buffer and depth accesses explicitly, so the
FrameGraph versions consumed by deferred lighting include the phase-2 overlay.

The renderer uploads GPU scene deltas through destination-offset staging copies;
unchanged slots do not cause a full scene upload. Dirty CPU bytes are queued and
recorded as copies in the current graphics frame command buffer, and their
staging allocations remain alive until that frame slot's fence completes. The
generic `GpuUploader` one-shot path is still synchronous for initialization and
resource loading, but the GPU Scene hot path no longer waits on the transfer
queue for each range.

Visibility/indirect scratch buffers are allocated per frame-in-flight and the
renderer selects the matching set before recording. This prevents a later
frame's culling pass from overwriting counters still consumed by an earlier
frame. GPU stage timestamps (frustum, indirect, Hi-Z, and two-phase) are
reported through the regular performance CSV fields when timestamp queries are
available.

Descriptor allocation is frame-slot scoped. The renderer waits for the slot
fence before selecting `m3DescriptorPools[currentFrame]` and calling
`vkResetDescriptorPool`; all descriptor sets recorded for that slot have
finished before they are recycled, so per-frame allocations remain bounded.

Scene-buffer growth is deliberately simple during development: when a scene
outgrows the current capacity, `VulkanGpuSceneBuffers::ensureCapacity()` waits
for the device, creates larger SoA/scratch buffers, and rebinds the next
frame's descriptors; the caller then performs a full scene re-upload. The
`vkDeviceWaitIdle` call is a visible whole-device stall and should be replaced
with a retired-buffer handoff before performance sign-off. This growth path is
separate from the normal dirty-range upload ring described above.

## Known performance scope

The indirect builder uses two compute dispatches (link and emit) and a small
per-frame scratch set (`meshHeads`, `meshNext`, and the compacted slot list).
Two-phase frames keep separate compacted slot lists for phase 1 and phase 2;
the phase-2 overlay therefore cannot overwrite the `firstInstance` data that
the next use of the phase-1 indirect commands will consume. The GPU vertex
shader reads Vulkan's `InstanceIndex` (DXC's `SV_InstanceID` mapping), which
includes the indirect command's `firstInstance` value under the current DXC
compile flags.
For N visible instances sharing one mesh this produces one command with
`instanceCount = N`; command count therefore tracks visible meshes rather than
visible instances. Two-phase occlusion is implemented but intentionally opt-in
while GPU validation and capture coverage are expanded.
For B13/B17 the development path copies the frustum/phase-2 slot-index lists
into a per-frame readback buffer and compares their union against a CPU
reference set built from the same world-space bounds and clip planes. The
result is exposed as `gpu_visibility_missing_count` and
`gpu_visibility_validation_passed` in `FrameStats`/performance CSV. In
GPU-driven mode the G-buffer also carries a debug `R32Uint InstanceId` MRT
(encoded as `slot + 1`, with zero reserved for clear/background). That image
is copied asynchronously into a frame-slot readback buffer and scanned after
the slot fence completes; out-of-range IDs are reported as
`gpu_instance_id_invalid_pixels`. The `scripts/run_m4_visibility.ps1` audit
requires both visibility checks to pass. For the strict reference-vs-occlusion
set comparison, `scripts/run_m4_instance_id.ps1` runs a frustum-only reference
and a two-phase pass with fixed timestep, then compares the per-frame ID report
files and emits a compact summary CSV.

Two-phase occlusion is opt-in (`--two-phase-occlusion`). Its structural
readback audit intentionally does not require every frustum candidate to
survive: candidates rejected by a valid Hi-Z test are expected. The InstanceId
comparison tool is the pixel-level check for visible-object coverage. A mixed
frame is supported: bindless-compatible opaque slots use GPU indirect
submission, while transparent, double-sided, or otherwise incompatible slots
are filtered out of the compute list and drawn by the CPU fallback. The GPU
path batches only the compatible subset, so fallback materials do not disable
GPU-driven rendering for the rest of the scene.
