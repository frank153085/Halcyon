# Halcyon Third-Party Dependencies

Third-party libraries are kept as independent Git submodules. The repository records an exact
commit for every submodule in the superproject, so a checkout can be reproduced with:

```text
git submodule update --init --recursive
```

Halcyon does not modify upstream sources or reuse their build scripts. The local
`ThirdParty/CMakeLists.txt` exposes small `Halcyon::` targets and disables optional upstream tests,
examples, tools, and installation rules by default.

## Dependency groups

| Milestone | Libraries | Main use |
| --- | --- | --- |
| M1 | Vulkan Memory Allocator, stb, robin-map | Vulkan allocation, image loading, compact hash tables |
| M2 | Dear ImGui, GoogleTest, SPIR-V Tools, SPIR-V Headers | Debug UI, tests, shader validation and inspection |
| M3 | cgltf, MikkTSpace, Basis Universal | glTF loading, tangent generation, compressed textures |
| M4 | meshoptimizer, Draco | LOD and meshlet preparation, compressed geometry |
| M5 | Google Benchmark, zstd | Performance regression tests and cooked-cache compression |

## CMake options

The M1 header-only targets are always available. Compiled or larger dependencies are opt-in:

```text
-DHALCYON_ENABLE_GOOGLETEST=ON
-DHALCYON_ENABLE_IMGUI=ON
-DHALCYON_ENABLE_SPIRV_TOOLS=ON
-DHALCYON_ENABLE_BASISU=ON
-DHALCYON_ENABLE_MESHOPTIMIZER=ON
-DHALCYON_ENABLE_DRACO=ON
-DHALCYON_ENABLE_BENCHMARK=ON
-DHALCYON_ENABLE_ZSTD=ON
```

The options are intentionally independent so an experiment can enable one dependency without
changing the M0/M1 renderer build.

## Pinned revisions

The following revisions are recorded as submodule pointers in the superproject. The original
license or notice file remains inside each submodule and must be shipped when the dependency is
redistributed.

| Path | Commit | License or notice |
| --- | --- | --- |
| `vkmemalloc` | `3aa921224c154a0d2c43912bc88e1c42ce1f7607` | `LICENSE.txt` |
| `stb` | `2c980bb59875b0d32144a71867fbdebb2f77cd20` | `LICENSE` |
| `robin-map` | `91362aab8f2c63ef90bf7c21fb2c3283ded5ae48` | `LICENSE` |
| `imgui` | `e0537bf72bd6da11a62b47020fd97cf5da0e3110` | `LICENSE.txt` |
| `googletest` | `d94a7326e97c9d564950e36c48ac402ba4933985` | `LICENSE` |
| `spirv-headers` | `496543121ce6419f23d6fa5d7194ba66c36212d2` | `LICENSE` |
| `spirv-tools` | `3a2f6ea08f17f149908a17ff9ba6cba4791d7f96` | `LICENSE` |
| `cgltf` | `85cd62382dfea638278962690cf515023f33ed00` | `LICENSE` |
| `mikktspace` | `3e895b49d05ea07e4c2133156cfa94369e19e409` | Repository license files |
| `basisu` | `99f52d63aa6799cbdaecfe977111dc5ec3b31d47` | `LICENSE` |
| `meshoptimizer` | `661f8626c0bf7e49dd139254e09ab93abf4f4a59` | `LICENSE` |
| `draco` | `052a31124c75e64dfbf4f9bc0df066e626aeef41` | `LICENSE` |
| `benchmark` | `04b5f41ec7e3b68b28a2379bc19804a48953117d` | `LICENSE` |
| `zstd` | `10da6ba6de05e29169261fa4b68eb99239f770dd` | `LICENSE`, `COPYING` |

## License handling

Keep each submodule's original license and notice files. Before distributing a binary, generate a
third-party license bundle and verify the terms for the exact commits recorded by Git. In
particular, zstd offers BSD and GPLv2 licensing options; select and document the applicable terms.
