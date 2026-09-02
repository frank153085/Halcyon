# FrameGraph layout

The directory follows the layout of `filament/src/fg` so that concepts can be
looked up by the same filename:

| Filament | Halcyon |
| --- | --- |
| `FrameGraph.{h,cpp}` | `FrameGraph.{h,cpp}` |
| `FrameGraphId.h` | `FrameGraphId.h` |
| `FrameGraphPass.{h,cpp}` | `FrameGraphPass.{h,cpp}` |
| `FrameGraphResources.{h,cpp}` | `FrameGraphResources.{h,cpp}` |
| `FrameGraphTexture.{h,cpp}` | `FrameGraphTexture.{h,cpp}` |
| `FrameGraphRenderPass.h` | `FrameGraphRenderPass.h` |
| `FrameGraphDummyLink.h` | `FrameGraphDummyLink.h` |
| `Blackboard.{h,cpp}` | `Blackboard.{h,cpp}` |
| `DependencyGraph.cpp` + `details/DependencyGraph.h` | same split |
| `PassNode.cpp` + `details/PassNode.h` | same split |
| `ResourceNode.cpp` + `details/ResourceNode.h` | same split |
| `Resource.cpp` + `details/Resource.h` | same split |
| `ResourceCreationContext.cpp` + `details/ResourceCreationContext.h` | same split |
| `details/ResourceAllocator.h`, `details/Utilities.h` | same split |

The canonical FrameGraph declarations and implementations live entirely in
this directory.  The former `Include/Halcyon` FrameGraph headers have been
removed so the source tree has a single authoritative location.  Unlike
Filament, Halcyon does not depend on a backend `DriverApi`; native allocation
is represented by `FrameGraphResourceProvider` and `FrameGraphNativeResource`.

The former `FrameGraphCompiler` / `FrameGraphExecutor` translation units and
the flat `*Node` / `VirtualResource` forwarding headers were empty wrappers;
they were removed.  Compilation and execution now remain in the matching
`FrameGraph.cpp` and the real node/resource code is under `details/`.

The compiler still uses Halcyon's compact index-based dependency storage.  The
Filament-shaped node/resource classes are kept in `details/` so backend and
tooling work can grow without putting implementation details in the public
FrameGraph header.  Filament's optional `FgviewerManager` is intentionally not
present because Halcyon has no fgviewer backend.
