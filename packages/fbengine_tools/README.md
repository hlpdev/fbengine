# fbengine_tools

fbengine's tools: the cooker, which makes a game's source files into what its release loads
fastest, and puts them in a pak, each file at the path it had, since the engine knows a file's
format by its bytes, not its name.

- Images (PNGs and JPEGs) become KTX2s of GPU blocks with their mips, made in linear light: BC7 by
  default, which every desktop GPU reads, or ASTC 4x4, Basis Universal's UASTC, or the
  texels as they are, as each one's sidecar (`wall.png.toml`, fbengine's `ImageSettings`) says.
- Every other file, sidecars too, goes in the pak as it is.

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

A game's release ships fbengine's DLL, never this one.
