# fbengine's architecture

How the engine is built, across its packages. FutureBlitz's docs/engine.md has the goals, the
packages, and the milestones; this doc has how the pieces fit, and docs/plan.md the steps. What waits
on the user is in docs/questions.md.

fbengine is its own engine, not a modern copy of Blitz3D's (the user, 2026-10-31). The
yardstick: it has to cover every engine feature SCP: Containment Breach Ultimate Edition v5
needs, so a remake indistinguishable to play can be built on it later (docs/research/), and be a
good modern engine on its own. Every structural choice is checked against both.

## Layers

```
 game code (FutureBlitz)
   │  imports
   ▼
 fbengine, fbengine_audio, fbengine_physics, fbengine_ui     ← FB: the API, objects with methods
   │  Extern "C" through fb-bindgen's bindings (c/*.fb)
   ▼
 fbengine_native, fbengine_audio_native, fbengine_physics_native   ← C++23 DLLs behind C APIs
   │  statically linked
   ▼
 SDL3, Vulkan (volk, VMA), fastgltf, KTX, stb, miniaudio, Jolt, meshoptimizer
```

- **What goes in C++**: everything per frame and per object in bulk: rendering, culling, skinning
  and animation sampling, collisions and picks, physics, mixing, decoding assets.
- **What goes in FB**: the API's shape, everything a game calls once per thing per frame, glue
  between packages, fbengine_ui, fbengine_tools, and the editor. Code that isn't hot
  and is easier to write in FB is written in FB; that's also the engine dogfooding the language.
- A C++ DLL is called only by its own package's FB code; packages meet in FB (physics reads an
  entity's transform through fbengine's FB API and hands Jolt numbers). No C++ links across
  packages.

## The C API

- Functions are `<prefix>_<area>_<verb>`: `fbe_entity_set_position`. Every type is fixed-size, so
  one checked-in binding is every target's. Structs go by pointer, never by value, so a Callback
  never needs a Struct by value (FutureBlitz's deferred work).
- **Handles**: an engine object is a `uint64_t`: a 32-bit slot, an 8-bit kind, and a 24-bit
  generation. 0 is none. Each pool (worlds, entities, meshes, textures, materials, sounds, bodies,
  ...) has its own kind, so a handle given to the wrong kind of call is refused, never taken for
  another object; a freed slot's generation goes up, so an old handle is caught, never aliased,
  and a slot whose generation would wrap is retired. Kinds are listed in one place,
  `native/common/handles.hpp`, each package in a range of its own so two never share one:
  fbengine 1 to 63 (World 1, entity 2, geometry 3, material 4, then as its milestones add pools),
  fbengine_audio 64 to 95, fbengine_physics 96 to 127, fbengine_video 128 to 143.
- **OS devices** go by the OS's own IDs, not pool handles: gamepads and joysticks by SDL's
  instance IDs, which SDL never reuses in a run, so a stale one names no device, and the FB side
  wraps each in an object of its own; displays by their place in the OS's list, read afresh.
- **Errors**: a call that can fail returns `bool` or a 0 handle, and `fbe_error()` says why, per
  thread. The FB side throws a typed Error for what a game should handle (a file that won't load,
  a mode that can't be set) and panics for a programming mistake (a freed handle, a value out of
  range), with the object's kind and the call in the message.
- **Batches** where a game touches many objects a frame: `fbe_entity_set_transforms(handles,
  matrices, count)`, so physics and animation sync is one call, not thousands.
- Headers are split by area (`fbe_window.h`, `fbe_input.h`, `fbe_scene.h`, ...) under one
  `fbengine.h`, and each gets its own binding file, so lanes don't edit the same files.

## The FB API

- Decided (2026-10-31): `@MainThread` Functions and Methods, no Properties; Structs' Fields for
  plain data; Enum members UPPER_CASE; checked Errors for what can fail at run time.
- **An object per handle**: the FB side keeps one wrapper per live handle in a table, so
  `CameraPick` returns the same `Entity` object the game made, and `==` and Map keys work.
  `Free()` frees the engine object and drops the table's entry; the wrapper stays, and using it
  panics ("this Mesh was freed").
- Engine Classes are Final, and a game attaches its own object with `entity.SetData(obj)` and
  `entity.Data()`, rather than extending `Entity` (the user, 2026-10-31). That keeps the wrapper
  table simple, and picks hand back the game's object through it.
- **Units and axes are std.math's** (the user, 2026-10-31): right-handed, Y up, -Z forward,
  radians, Vec3, Quat, Mat4, with `Deg()` where a game thinks in degrees
  (`cube.Turn(0, Deg(1), 0)`).

## The scene

- **The game's view is a tree of entities**: `Pivot`, `Camera`, `Light`, `Mesh`, `Sprite`,
  `Terrain`, each with a parent, a local transform, a name, and its render, pick, and collision
  settings.
- **Inside, it's data-oriented**: transforms in arrays with dirty flags, world matrices updated
  once a frame for what moved, components (mesh renderer, light, camera, collider) in pools
  indexed by entity. No ECS is exposed; a game never sees the arrays.
- **Worlds**: entities live in a `World`. There's a default one, so a small game never names it,
  and a game or the editor can make more (a preview scene, a scene rendered to a texture). A Camera
  renders the World it's in.
- `UpdateWorld(seconds)` steps animation and the scene's collisions; `RenderWorld()` draws every
  camera; both are the game's to call, in its own loop. A fixed-step helper (`FixedStep(rate)`)
  gives a game logic at a fixed rate with tweening between steps, as SCP:CB's needs.
- **Collisions and picks** are the scene's own, for games that don't need a physics engine:
  swept spheres and ellipsoids against meshes, boxes, and spheres, with stop and slide responses,
  step-up, and ray and sphere picks, in C++ on the job system. SCP:CB's player and NPCs move this
  way; a game wanting rigid bodies uses fbengine_physics.

## The renderer

- **Vulkan 1.2** with dynamic rendering, synchronization2, and descriptor indexing (bindless
  textures), through volk and VMA; MoltenVK on macOS supports that set. Two frames in flight.
- **What it runs on** (the user, 2026-10-31): Vulkan 1.2 GPUs (2016 onward), Windows 10, Linux
  with glibc 2.28, macOS 13 on MoltenVK. **What it aims for** (the user, 2026-10-31): a scene of
  SCP:CB's scale in the high hundreds of fps, 500 or more at 1080p on the user's RTX 4080 and
  well over 144 on a GTX 1060-class GPU, so about 2 ms a frame for the engine, CPU and GPU: batched
  draws, culling, no per-object overhead per frame. Each milestone measures its test scenes
  against it.
- **A small frame graph**: each frame's passes (shadows, depth prepass, opaque, sky,
  transparent, post, 2D overlay) declare what they read and write; the graph orders them, places
  barriers, and reuses transient targets.
- **Extract, then draw**: after UpdateWorld, the scene is read into render lists (visible
  objects, sorted), so drawing never reads the live scene; a render thread can take drawing over
  later without changing the API.
- **One material system, three shading models**: PBR, UNLIT, and VERTEX (per-vertex lighting,
  for a retro look). Texture layers with blend modes and their own UV sets, lightmaps (SCP:CB's
  rooms are lightmapped), fog, and alpha all go through the same pipelines, as specialization
  constants, so any mix of them draws in the same pass.
- **Lights**: clustered forward, so the many small lights of a facility work, per pixel or per
  vertex. Shadows come with E6.
- **Render to texture**: any Camera can render into a Texture (security monitors, mirrors,
  portals), and post effects run per camera.
- **2D** draws after 3D, in the same frame graph, into the window or into a Texture; pixel
  access (`Lock`, `ReadPixel`, `WritePixel`) works on a CPU copy, uploaded or read back once per
  frame, never per pixel.
- **Text** is FreeType's rasterizing and HarfBuzz's shaping, statically linked, into glyph
  atlases (the user, 2026-10-31).
- **Names**: fbengine's GPU image is `Texture`, made from a file or a std.image `Image`, and its
  `Font` lives in its 2D module, qualified where a file imports both (the user, 2026-10-31).
- **Shaders** are Slang, compiled to SPIR-V when the engine's built and embedded in the DLL
  (the user, 2026-10-31).
  Games' own shaders (E6) come with the asset pipeline's build step.

## Assets

- Paths are std.vfs's (`res://`), with folders in development and pak files in a release.
- Loaders are C++ (fastgltf, KTX, stb_image), on worker threads.
  `Assets.Load<T>(path)` loads now; `Await Assets.LoadAsync<T>(path)` loads on a worker, and the
  Task completes on the main thread at a Flip.
- A cache keyed by normalized path hands out the same asset again, refcounted; `Free()` and the
  last reference let it go.
- The cooker, typed references, and hot reload are E8's, and need a package build step from
  FutureBlitz (docs/questions.md).
- The engine loads modern formats only: glTF for meshes, scenes, and animation; KTX2, PNG, and
  JPEG for textures; OGG, WAV, and FLAC for sound. Converting anything else is a game's own
  tooling, outside the engine and the language (the user, 2026-10-31).

## Threads

- The API is the main thread's (`@MainThread`). Inside, the C++ core has one job system, a
  worker per core less one, which asset decoding, culling, animation, and collisions use;
  Jolt runs on its own pool.
- Nothing calls into FB from a worker; finished work is queued and handed over at Flip.

## How packages work together

- `Frame.OnFlip(hook)`: a package registers what it does each frame (audio moving its listener
  with a camera, physics syncing bodies to entities), so a game doesn't call each package's
  update itself. Hooks run in Flip, in registration order, before `Scheduler.Main.Step()`.
- fbengine_audio's 3D sound and fbengine_physics' bodies follow entities through fbengine's FB
  API, read in a batch per frame.

## Testing

- C++ unit tests (Catch2) per area; FB tests (`fb test`) per package; examples that ctest runs.
- **No windows**: SDL's offscreen video, and Vulkan on lavapipe (Linux and Windows) and MoltenVK
  (Apple silicon) in CI.
- **Golden images**: a test renders on lavapipe, which is the same on every machine, and compares
  with a checked-in PNG within a tolerance (per pixel, and how many may differ). A tool rewrites
  them on purpose, and a changed image is reviewed like code.
- Performance budgets per milestone, measured on the user's machine.
