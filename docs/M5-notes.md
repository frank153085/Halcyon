# M5 Virtualized Geometry V1

M5 uses the Stanford Computer Graphics Laboratory Lucy scan as a research-only
fixture. The source archive is fetched by `scripts/fetch_m5_assets.ps1` and is
never committed. The fetch helper publishes the local PLY under the ignored
`assets/models/lucy/` fixture directory so the example's compiled asset root
can load it directly. Redistribution requires Stanford attribution and is not
for commercial use.

The PLY path is decoded by tinyply at commit
`c9bb690dfe5e9105961e9e28120c48c9ae084bc6`; glTF/GLB continues to use fastgltf.
Position and normal properties are requested as separate typed buffers; when
all three normal components are absent (or incomplete), the loader generates
area-weighted vertex normals before cooking meshlets. PLY materials use the
default generated PBR textures.
meshoptimizer `661f8626c0bf7e49dd139254e09ab93abf4f4a59` builds fixed LOD ratios
`1.0/0.5/0.25` and meshlets capped at 64 vertices and 124 triangles. zstd uses
the checked-in revision and a fixed compression level with frame checksums.

`*.halcyon.vgcache` is a private, little-endian, versioned ABI. Schema v3 has a
fixed 104-byte header and stores the source SHA-256, build limits,
meshoptimizer version, simplify error, all three LOD ratios, and the payload
offset, compressed size, and decompressed size. The table counts live at the
start of the checksummed zstd payload. Readers reject stale hashes,
truncation, overflow, out-of-range references, unsupported limits, and invalid
checksums. The reader also requires every enabled LOD ratio for every primitive
in deterministic primitive-major order; partial LOD sets are rejected instead
of being interpreted with a different runtime LOD index. No timestamps or
random identifiers are written.

The runtime prefers a matching `.halcyon.vgcache` sidecar for PLY sources,
rebuilds in memory when absent or stale, and keeps the conventional indexed
mesh as the fallback. glTF and GLB remain on the established fastgltf/indexed
path and do not require a virtual-geometry sidecar. Transparent, alpha-masked,
double-sided, and otherwise incompatible materials remain on the existing CPU
or deferred path. VirtualGeometryIndexed is selected only when the device and
shader ABI support its meshlet metadata, visibility IDs, and indirect buffers.

## Visibility and shading ABI

The primary visibility attachment is `R32_UINT`: zero means background; bits
0-7 carry the instance slot, bits 8-27 carry `meshletIndex + 1`, and bits 28-31
carry the four-bit material class. Two additional `R32_UINT` attachments carry
`triangleIndex + 1` (the CPU compatibility name is `primitive`) and packed
16-bit `(barycentric.x, barycentric.y)` values.
The reserved `+1` encoding keeps a valid first meshlet/triangle distinct from a
cleared pixel. Compute shading validates every decoded table reference before
loading indices or vertices, then interpolates position, normal, and UV from
the three indices. Lucy uses factor-only materials, so the reconstructed UV is
currently validated and kept available for a future texture-backed material
ABI. Meshlet metadata is a fixed 80-byte GPU stride (the final 12 bytes are
padding matching the host upload structure); the per-frame indirect stream is
bounded at `2^20 - 1` meshlets because the visibility ID reserves zero for the
background and stores `meshletIndex + 1` in a 20-bit field; the Lucy fixture is not truncated at the LOD0
submission boundary.

The internal indirect-draw token is separate from the attachment encoding:
bits 0-19 carry the zero-based meshlet index and bits 20-27 carry the instance
slot. `firstInstance` forwards this token to `visibility.vert`; the shared
`virtual_geometry_ids.hlsli` helpers and `VirtualGeometry.h` constants define
both sides of this ABI.

| Attachment | Encoding | Consumer |
| --- | --- | --- |
| `VisibilityBuffer` | `instance[7:0]`, `(meshlet + 1)[27:8]`, `materialClass[31:28]` | material classification and shading |
| `VisibilityPrimitive` | `triangle + 1` (`0` is background) | shading triangle lookup |
| `VisibilityBarycentrics` | `bary.x` low 16 bits, `bary.y` high 16 bits, UNORM | shading attribute interpolation |

Material classification writes the visibility class in the high four bits and
the complete 28-bit material-table index in the low bits, so class bucketing
does not truncate material references. Unsupported or malformed classes write
`0xffffffff`; compute shading treats that sentinel as background. The cooker
and cache validator reject primitive material indices above `0x0fffffff` so a
cache cannot contain references that the packed classification ABI cannot
represent.

The compute shading push-constant ABI carries camera position, the previous
view-projection matrix, and a fallback directional-light vector while staying
within the Vulkan-guaranteed 128-byte push-constant budget. Actual instance transforms and mesh/material rows are uploaded into
per-frame buffers in `FramePacket` order, which keeps visibility instance IDs
stable even when persistent GPU-scene slots are sparse. Normal reconstruction
derives the inverse-transpose 3x3 from that transform buffer. Per-frame GPU
readback exposes visible meshlet, generated indirect-command, and rejected
visibility-record counts through `FrameStats` and the performance CSV. The
rejected-record count must remain zero for a valid VirtualGeometryIndexed frame.

The visibility vertex push constants remain 80 bytes: the trailing `uint4`
stores meshlet, vertex, instance, and meshlet-vertex-table counts for bounds
checks. The 16-byte indirect-build constants include the global index count,
and classification uses a 32-byte block containing the selected asset's
meshlet count, meshlet-vertex-table count, index-table count, and expected
dense mesh ID. It also binds the packed meshoptimizer triangle-byte stream via
`ByteAddressBuffer` and validates each local corner before shading.
Classification rejects records whose instance row does not match that selected
asset. The GPU copy of this byte stream is zero-padded to a 4-byte boundary
for safe word loads; the on-disk cache remains byte-packed.
Invalid IDs and indirect ranges terminate before any table load.

Primitive material indices stored in the cache remain source-asset indices.
They are used only to prove that the virtual asset has one uniform material;
classification uses the renderer-dense material ID from the instance table,
so cache indices are never compared with runtime descriptor-table indices.

LOD selection in M5 is deliberately deterministic and distance based: the
first two transitions occur at 8x and 20x the asset bounding-sphere radius,
selecting the fixed 1.0/0.5/0.25 LOD set. Hi-Z occlusion samples the previous
frame's minimum-depth pyramid only when both the camera matrix and the
instance transform are byte-identical to the previous packet; otherwise the
meshlet survives conservatively. The pyramid is rebuilt after visibility
rasterization for the next frame.

Normal-cone culling uses the meshoptimizer object-space cone only for
approximately uniform, orthogonal instance scales. Non-uniform and sheared
transforms skip the cone test and retain frustum/Hi-Z results so the
object-space cone cannot over-cull. Frustum planes remain normalized in world
space and the cached sphere radius is multiplied by a conservative transformed
radius, which keeps non-uniform and sheared instances from being over-culled.
Hi-Z projection uses the exact maximum scale for orthogonal TRS and a
Frobenius-norm upper bound for shear.
The cache stores `cone_axis.xyz` and meshoptimizer's `cone_cutoff`; the
perspective sphere form is `dot(center - camera, axis) >= cutoff * distance +
radius`. A cutoff of 1 denotes the intentionally degenerate, non-useful cone.

The compute descriptor set binds visibility/primitive/barycentric images,
material classifications and the validation counter, HDR and motion storage, meshlet/triangle/index/vertex tables, the three
procedural IBL images, the shared linear sampler, the material table, and the
frame light buffer. DeferredIndexed and GpuDrivenIndexed use their existing
descriptor sets and render passes; these bindings are only created and consumed
by VirtualGeometryIndexed. All virtual shaders consume the storage structures
from `virtual_geometry_ids.hlsli`. Pipeline creation reflects each
`StructuredBuffer`/`RWStructuredBuffer` `ArrayStride` and compares it with the
corresponding C++ type, in addition to the existing descriptor and push-constant
checks. A meshlet, vertex, transform, material, light, or indirect-command ABI
drift therefore rejects the virtual pipeline before recording a frame.

Devices without `fragmentShaderBarycentric`,
sampled/color-attachment support for `R32_UINT`, sampled/storage support for
`R32_SFLOAT` Hi-Z, or sampled/storage/transfer-destination support for HDR
`RGBA32F` and motion `RG16F`,
descriptor indexing, the per-stage/set descriptor counts required by the
16-binding shading ABI, or
indirect-count/first-instance support are downgraded before pipeline creation. The runtime
also checks `maxDrawIndirectCount`, `maxStorageBufferRange`, and the 32-bit
vertex/index encoding limits before accepting an uploaded virtual asset. Such
devices or assets are downgraded before pipeline
creation, so the other two paths remain usable.

Virtual Geometry reconstructs camera motion from the visibility pixel and the
previous view-projection matrix, then uses the shared TAA, ACES tonemap, and
present passes. Compute Shading evaluates the same metallic-roughness BRDF and
IBL as the baseline for directional, point, and spot lights; the M5 path does
not bind the deferred CSM shadow-map ABI, so its direct-light result is
unshadowed in V1. M5 V1 accepts static instances sharing one
virtual asset with one primitive material (the Lucy fixture). The source
primitive material index and the runtime dense material slot intentionally
remain separate namespaces; the instance table supplies the latter to
classification. A packet containing a different virtual
asset, a meshlet-instance product above the visibility-buffer capacity, more
than 256 instances, a mirrored (negative-determinant) transform, or any transparent,
alpha-masked, double-sided, CPU-fallback,
missing, or GPU-upload-incomplete instance falls back for the whole packet
rather than silently dropping scene content.

Lucy captures should record fixed camera, exposure, timestep, resolution,
driver, and GPU model together with visible meshlet count, indirect count,
SSIM/FLIP, and CSV timings. `scripts/run_m5_lucy_acceptance.ps1` is the
single-entry capture/metrics/optional-RenderDoc workflow. Pass
`-RequireImageMetrics` when the acceptance run is the evidence-producing gate;
the SSIM gate defaults to `0.995` and can be adjusted with `-SsimThreshold`.
Without it, missing external SSIM/FLIP tools are recorded as `skipped` so
offline/basic CI remains usable. Large-asset tests are
skipped explicitly when the archive is unavailable; checked-in procedural tests
remain mandatory. When `HALCYON_ENABLE_BENCHMARK=ON`, the benchmark target
always covers the small deterministic grid and adds Lucy meshlet-build and
cache round-trip cases when `HALCYON_LUCY_PLY` (or the checked-in asset-root
path) is available.

The per-frame CSV also contains `render_path`, which identifies the backend path
actually recorded for the frame (`DeferredIndexed`, `GpuDrivenIndexed`, or
`VirtualGeometryIndexed`). This makes packet-level Virtual Geometry fallbacks
visible in the results; GPU-driven compatibility mode is normalized to
`GpuDrivenIndexed` in this field. The acceptance runner uses at least 2100
frames because the application discards the first 300 warm-up frames and
records the following 1800 frames for percentile and per-frame performance
data.
