# Questions for the user

What waits on the user's call, each with a recommendation. Answered ones move to FutureBlitz's
docs/decisions.md and leave this file.

## Before the first lanes

1. **Can a game extend engine Classes?** Recommended: no; engine Classes are Final, and a game
   attaches its own object with `entity.SetData(obj)` and `entity.Data()`. One wrapper per handle
   stays simple, and picks hand back the game's object through it. The alternative, extending
   `Entity` (`Class Door Extends Pivot`), is nicer to write but needs the engine to make the
   game's Class for objects it creates (loaded scenes, copies).
2. **Shaders**: recommended Slang, compiled to SPIR-V at build time (one language for Vulkan and,
   through MoltenVK, Metal; modules and generics; Khronos hosts it). The alternative is GLSL with
   glslang, simpler and older.
3. **Text**: recommended FreeType for rasterizing and HarfBuzz for shaping (every script, kerning,
   ligatures), statically linked. The alternative, stb_truetype, is smaller but has no shaping or
   hinting.
4. **2D type names**: std.image already has `Image` (pixels on the CPU) and its fonts.
   Recommended: fbengine's GPU image is `Texture`, made from a file or a std.image `Image`, and
   2D draws Textures; fbengine's own `Font` lives in its 2D module, so the names don't clash in
   a file that imports both, or fbengine names it `TextFont`.

## Before E2

5. **Minimum hardware and OSes**: recommended Vulkan 1.2 GPUs (2016 onward, with dynamic
   rendering and synchronization2), Windows 10, glibc 2.28 Linux (as fb), macOS 13 on MoltenVK.
6. **Performance targets**: recommended 60 fps at 1080p on a GTX 1060-class GPU for a scene of
   SCP:CB's scale, and as a budget for test scenes, 144 fps on the user's RTX 4080.

## Before E8

7. **FutureBlitz's package build step** (FutureBlitz's open-questions.md): the cooker and typed
   references (`Res.Textures.Wall`) run at build time. Recommended: a package names a tool of its
   own in project.toml that `fb build` runs before compiling, whose output files join the build.
   It's a language-tooling change, in a FutureBlitz release.
8. **Re-exports** (`Public Import`): only needed for an umbrella `fbengine_all`. Recommended:
   leave it.

## Before E12

9. **Video decoder**: recommended pl_mpeg (MPEG-1, one header, MIT), with a game shipping its
   videos as MPEG-1; or dav1d (AV1) for quality, at many times the size.

## Before E13

10. **The editor's scope**: recommended a scene and asset editor (scene view, tree, inspector,
    asset browser, play in the editor) built on fbengine_ui, with code staying in VS Code and
    fb-lsp; FutureBlitz's own IDE is a separate, later project. The inspector needs FutureBlitz to
    set reflected Properties (its Deferred work).

## When to publish

11. **The packages on the production registry**: recommended at E5, as 0.x, once a game can draw
    a scene; staging until then.
