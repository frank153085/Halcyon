# M5 Virtualized Geometry V1

M5 uses the Stanford Computer Graphics Laboratory Lucy scan as a research-only
fixture. The source archive is fetched by `scripts/fetch_m5_assets.ps1` and is
never committed. Redistribution requires Stanford attribution and is not for
commercial use.

The PLY path is decoded by tinyply at commit
`c9bb690dfe5e9105961e9e28120c48c9ae084bc6`; glTF/GLB continues to use fastgltf.
meshoptimizer `661f8626c0bf7e49dd139254e09ab93abf4f4a59` builds fixed LOD ratios
`1.0/0.5/0.25` and meshlets capped at 64 vertices and 124 triangles. zstd uses
the checked-in revision and a fixed compression level with frame checksums.

`*.halcyon.vgcache` is a private, little-endian, versioned ABI. It stores the
source SHA-256, build limits, meshoptimizer version, simplify error, table
counts, and a checksummed compressed payload. Readers reject stale hashes,
truncation, overflow, out-of-range references, unsupported limits, and invalid
checksums. No timestamps or random identifiers are written.

The runtime prefers a matching PLY sidecar, rebuilds in memory when absent or
stale, and keeps the conventional indexed mesh as the fallback. Transparent,
double-sided, and otherwise incompatible materials remain on the existing CPU
or deferred path. VirtualGeometryIndexed is selected only when the device and
shader ABI support its meshlet metadata, visibility IDs, and indirect buffers.

Lucy captures should record fixed camera, exposure, timestep, resolution,
driver, and GPU model together with visible meshlet count, indirect count,
SSIM/FLIP, and CSV timings. Large-asset tests are skipped explicitly when the
archive is unavailable; checked-in procedural tests remain mandatory.
