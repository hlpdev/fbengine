# fbengine_tools

fbengine's tools: the cooker, which makes a game's source files into what its release loads
fastest, and puts them in a pak, each file at the path it had, since the engine knows a file's
format by its bytes, not its name.

- Images (PNGs and JPEGs) become KTX2s of GPU blocks with their mips, made in linear light: BC7 by
  default, which every desktop GPU reads, or ASTC 4x4, Basis Universal's UASTC, or the
  texels as they are, as each one's sidecar (`wall.png.toml`, fbengine's `ImageSettings`) says.
- glTFs (named `.gltf` or `.glb`) become binary glTFs of one buffer. meshoptimizer orders their
  triangles and vertices for the GPU's caches and fetches, unless their sidecar (fbengine's
  `ModelSettings`) says `optimize = false`. A glTF the engine wouldn't load stops the cook.
- Shaders (named `.slang`, but for a `.inc.slang`, which is what they include) are compiled, with
  the engine's scene shader, into what `Assets.Load<Shader>` loads, in debug builds too. They need
  Slang's `slangc`, of the one release the engine's are compiled with, 2026.20, on the PATH or named by
  `FBENGINE_SLANGC`; a game with no shaders doesn't.
- Every other file, sidecars and buffers too, goes in the pak as it is.

A game names its build step, and keeps its files in `assets/`:

```toml
[dependencies]
fbengine = "0.1"
fbengine_tools = "0.1"

[build]
package = "fbengine_tools"
program = "step/game.fb"
```

Each build, the step writes the game's module `res`, its files' typed references and a `Mount`:

```vb
Import std.core
Import fbengine
Import "./res" As Res    // or "../res", with main = "src/main.fb"

Try Res.Mount()
Print(Try Assets.ReadText(Res.Readme))
```

A debug build's `Mount` mounts `assets/` at res://, and watches it, so what changes reloads as the
game runs. A release build's cooks `assets/` into `build/<name>.pak`, which the game ships beside
its executable, or in a macOS bundle's Resources, each file again only when it or its settings
change; `fb build -o` needs that pak copied beside what it builds, each time. What's hidden, its name starting with '.', and Windows' Thumbs.db and desktop.ini, are left
out. `Cooker` and `References` do the same for a game's own tools:

```vb
Import std.io
Import fbengine_tools

Try Cooker.CookFolder(Path("assets"), Path("build/game.pak"))
```

Its References write typed references to a game's files: a module of each one's res:// path, by
its folders and name. A game imports it as Res, so `Res.Textures.Wall` is a file's path, and a
name with a typo is a compile error, not a failed load.

```vb
Try References.Write(Path("game/res"), Path("game/src/res.fb"))
// And in the game, which imports "./res" As Res:
Local wall = Try Assets.Load<Texture>(Res.Textures.Wall)
```

A game's release ships fbengine's DLL, never this one. Released with fbengine at the same version,
and needs fb 1.2.3 or later. Apache-2.0. In its DLL:
- KTX-Software's writer is Apache-2.0 (notices/ktx.txt), with Basis Universal's encoder
  (notices/basisu.txt), the ASTC encoder, Apache-2.0 (notices/astc-encoder.txt), and zstd,
  BSD-3-Clause (notices/zstd.txt);
- meshoptimizer is MIT (notices/meshoptimizer.txt);
- fastgltf is MIT (notices/fastgltf.txt), with simdjson, Apache-2.0 or MIT (notices/simdjson.txt);
- stb is MIT or public domain (notices/stb.txt);
- on Windows, MinGW-w64's winpthreads is MIT and BSD-style (notices/winpthreads.txt).
