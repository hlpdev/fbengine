# fbengine

A game engine for [FutureBlitz](https://futureblitz.dev), as packages: any Blitz3D game can be
remade with it, looking nearly identical, and a modern game can be made with it from the start.

| Package | What's in it |
|---|---|
| `fbengine` | windows and input, 2D, the 3D scene, animation, the renderer, assets, Blitz's collisions |
| `fbengine_audio` | sound, 3D sound, and music |
| `fbengine_physics` | Jolt's rigid bodies, characters, and casts |
| `fbengine_ui` | a game's UI, and a debug UI |
| `fbengine_tools` | the asset cooker, pak files, and the mesh converter |

They're released together, at one version.

## Building

fbengine needs fb, CMake 3.28, and a C++23 compiler. Each package's DLL is built into its
`native/<target>/` folder, where the package finds it, and ctest runs the packages' tests and the
examples:

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

## License

Apache-2.0.
