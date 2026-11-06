# fbengine

A game engine for FutureBlitz: windows and input, 2D, the 3D scene, and a Vulkan renderer.

- **The app and input:** a window, by SDL3; `Flip` and frame timing; the keyboard by position, with
  text input and IME; the mouse, with relative mode for mouse look; gamepads and raw joysticks;
  input actions bound to keys, buttons, and axes, rebound and kept as TOML; message boxes and
  `OpenURL`; and `Simulate`, which drives all of it in tests with no window.
- **2D:** a canvas into the window or a texture; images with handles, rotation, and frames; text
  from TrueType fonts and the system's, shaped, bidirectional; pixels read and written; rectangle
  and pixel-exact collisions.
- **The scene:** worlds of entities in a tree; transforms; cameras; lights and fog; meshes built in
  code; materials; in-view and line-of-sight tests; swept collisions that slide; picks; sprites;
  terrain; `UpdateWorld` and `FixedUpdate`.
- **The renderer:** Vulkan through volk and VMA, on a frame graph; clustered lights; cameras into
  textures; PBR, normal maps, and image-based light; shadows; HDR and tonemapping; bloom, SSAO,
  FXAA, TAA, MSAA, colour grading, and a game's own passes; GPU particles, decals, and sky;
  instancing, LOD, and occlusion culling; materials with a game's own shaders.
- **Animation:** skinning on the GPU, clips with blending, layers, and events, bones under the
  game's control, morph targets, and node animation.
- **Assets:** a job system; res:// over folders and paks; glTF, KTX2, PNG, and JPEG loaded on the
  jobs; one asset per path, with sidecar settings; hot reload; and scene files as text.

It runs on a GPU with Vulkan 1.2, on Windows x64, Linux x64 and arm64 with glibc 2.28 or later, and
macOS arm64 and x64 through MoltenVK. On macOS, a player needs MoltenVK and Vulkan's loader, which
the package doesn't ship: Homebrew's `molten-vk` and `vulkan-loader` are what its tests use. It
needs fb 1.2.3 or later. How it fits together is in
[docs/architecture.md](https://github.com/hlpdev/fbengine/blob/master/docs/architecture.md), and
its scene files in [docs/scene-files.md](https://github.com/hlpdev/fbengine/blob/master/docs/scene-files.md).

```toml
[dependencies]
fbengine = "0.1"
```

```vb
Import std.core
Import fbengine

Print("fbengine {Version()}")
```

Its sound is `fbengine_audio`, its physics `fbengine_physics`, its UI `fbengine_ui`, and its video
`fbengine_video`, released with it at the same version. Apache-2.0. In its DLL:
- SDL is the zlib license's (notices/sdl.txt);
- FreeType is the FreeType License's (notices/freetype.txt);
- HarfBuzz is MIT's (notices/harfbuzz.txt), and its USE tables Microsoft's MIT
  (notices/harfbuzz-ms-use.txt);
- SheenBidi is Apache-2.0 (notices/sheenbidi.txt);
- stb is MIT or public domain (notices/stb.txt);
- KTX-Software's reader is Apache-2.0 (notices/ktx.txt), with Basis Universal's transcoder
  (notices/basisu.txt), the ASTC encoder, Apache-2.0 (notices/astc-encoder.txt), and
  zstd, BSD-3-Clause (notices/zstd.txt);
- fastgltf is MIT (notices/fastgltf.txt), with simdjson, Apache-2.0 or MIT (notices/simdjson.txt);
- volk is MIT (notices/volk.txt), Vulkan-Headers Apache-2.0 or MIT (notices/vulkan-headers.txt),
  and the Vulkan Memory Allocator MIT (notices/vma.txt);
- on Windows, MinGW-w64's winpthreads is MIT and BSD-style (notices/winpthreads.txt);
- Font.BuiltIn's font is a subset of Noto Sans, under the SIL Open Font License 1.1
  (notices/noto-sans.txt).

Portions of this software are copyright © 2026 The FreeType Project
(www.freetype.org). All rights reserved.
