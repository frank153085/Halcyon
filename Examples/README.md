# Standalone Examples

Every example is an independent executable target.  The shared host code lives
in `Examples/Common/ExampleRunner.cpp`; an example only supplies its entry
point and a `SceneManagerConfig`. Meshes, materials, textures, and instances
always enter the renderer through `SceneManager`; examples do not configure
backend startup resources.

## Build

Examples are enabled by default.  To build them explicitly:

```powershell
cmake --build out\build\windows-msvc-debug --target HalcyonExamples
```

The aggregate target is optional.  Individual targets can be built and run
without compiling the other examples:

```powershell
cmake --build out\build\windows-msvc-debug --target HalcyonExample01Triangle
cmake --build out\build\windows-msvc-debug --target HalcyonExample02TexturedModel
```

## Run

Each target has a unique output directory, so adding a newer example never
replaces an older executable:

```powershell
out\build\windows-msvc-debug\Examples\HalcyonExample01Triangle\HalcyonExample01Triangle.exe --frames 300
out\build\windows-msvc-debug\Examples\HalcyonExample02TexturedModel\HalcyonExample02TexturedModel.exe --frames 300
```

Both programs accept `--width`, `--height`, `--frames`, `--no-validation`, and
`--help`.

Example 01 supplies an in-memory `StaticScene`, while Example 02 references
`models/monkey/monkey.gltf` relative to the shared asset root. Both sources use
the same asset loading, SceneDatabase registration, instancing, and GPU upload
path.

## Add an Example

Create a directory under `Examples`, add a small `Main.cpp` that calls
`Halcyon::Examples::run`, and add a local `CMakeLists.txt` that invokes
`halcyon_add_example`.  Use a new target name for every example.  The helper
assigns an isolated runtime directory under `out/build/<preset>/Examples`.
Declare assets and instances on `ExampleDefinition::scene`; do not load files
or create renderer resources in an example callback.
