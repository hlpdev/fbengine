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
- Every other file, sidecars and buffers too, goes in the pak as it is.

```toml
[dependencies]
fbengine = "0.1"
fbengine_tools = "0.1"
```

```vb
Import std.io
Import fbengine
Import fbengine_tools

Try Cooker.CookFolder(Path("game/res"), Path("build/game.pak"))
// And in the game's release:
Try Assets.MountPak("res://", Path("game.pak"))
```

Its References write typed references to a game's files: a module of each one's res:// path, by
its folders and name. A game imports it as Res, so `Res.Textures.Wall` is a file's path, and a
name with a typo is a compile error, not a failed load.

```vb
Try References.Write(Path("game/res"), Path("game/src/res.fb"))
// And in the game, which imports "./res" As Res:
Local wall = Try Assets.Load<Texture>(Res.Textures.Wall)
```

A game's release ships fbengine's DLL, never this one.
