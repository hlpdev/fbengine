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
- **What edits the API reads it by reflection** (E13): a property is what has both an `X()` and a
  `SetX()` of one type, and a setter says what it takes by attributes, `@Limits`, `@ShownInDegrees`,
  `@NoAlpha`, and `@NotEdited`, the one place for a rule of one number, since a setter panics for what
  it can't take and the editor can't catch that; a rule of two, as a spot's cone's, the editor keeps by
  hand. An Enum is listed by what Load takes by number, as FutureBlitz can't list its members yet.
- **Components are data** (E13): FutureBlitz has no reflection at run time and no loading of a game's
  code, so the editor can't see a game's Classes. A component is a Class Load can make from nothing;
  `Components.Add<T>(name)` describes it by reflection; a game's components/main.fb ends with
  `Components.Describe()`, which the editor builds by fb and runs, once the person's said that folder's
  code may run, once per folder, as Play does; the editor edits plain Values, which a scene keeps; and
  `Components.Attach(entity, name, values)` makes one as a scene loads, checking `@Limits`, which
  `entity.Data<T>()` gives back, one of each type. fbengine_physics's bodies are one of these, so
  fbengine never names it.
- Engine Classes are Final, and a game attaches its own object with `entity.SetData(obj)` and
  `entity.Data()`, rather than extending `Entity` (the user, 2026-10-31). That keeps the wrapper
  table simple, and picks hand back the game's object through it.
- **Units and axes are std.math's** (the user, 2026-10-31): right-handed, Y up, -Z forward,
  radians, Vec3, Quat, Mat4, with `Deg()` where a game thinks in degrees
  (`cube.Turn(0, Deg(1), 0)`).

## The scene

- **The game's view is a tree of entities**: `Pivot`, `Camera`, `Light`, `Mesh`, `Sprite`,
  `Terrain`, each with a parent, a local transform, a name, and its render, pick, and collision
  settings. A Terrain's UVs are in its squares, u along x and v its size less z, so a map over it
  has its first row at the far edge, the reverse of its heights' order.
- **Inside, it's data-oriented**: transforms in arrays with dirty flags, world matrices updated
  once a frame for what moved, components (mesh renderer, light, camera, collider) in pools
  indexed by entity. No ECS is exposed; a game never sees the arrays.
- **Worlds**: entities live in a `World`. There's a default one, so a small game never names it,
  and a game or the editor can make more (a preview scene, a scene rendered to a texture). A Camera
  renders the World it's in.
- `UpdateWorld(seconds)` steps animation and the scene's collisions; `RenderWorld()` draws every
  camera; both are the game's to call, in its own loop. A fixed-step helper (`FixedUpdate(rate)`)
  gives a game logic at a fixed rate with tweening between steps, as SCP:CB's needs.
- **Collisions and picks** are the scene's own, for games that don't need a physics engine:
  swept spheres and ellipsoids against meshes, boxes, and spheres, with stop and slide responses,
  step-up, and ray and sphere picks, in C++ on the job system. SCP:CB's player and NPCs move this
  way; a game wanting rigid bodies uses fbengine_physics.
  - A mover goes from where it was at its World's last `Collide` to where it is now, as an
    ellipsoid swept in its own space, where it's a sphere: against meshes' triangles (their
    fronts), boxes (their twelve triangles), and ellipsoids, each a snapshot at its current
    transform. Both sides get the contact.
  - Where it differs from Blitz3D, on purpose: targets are where they are now, never swept, so
    one moving into a mover pushes it out only as far as nothing else is; squeezed between two
    facing each other, the mover's middle goes to the middle of the gap, never inside either,
    unless one closes faster than its radius in a frame and passes over its middle first. An
    ellipsoid target keeps both its radii; a box turns and scales with its entity. A mover whose
    touches use up its World's iterations (8 by default) is pushed out of every plane it still
    touches at once, where that leaves it less in them, and stops there, unless it's still
    further in than where the move began, as along a crease into a narrowing wedge: then it
    stays where it was.
  - The skin keeps a mover off what it touches: it backs away the way it came, never pushed off
    along the face's normal into something it was leaving, and goes on with what it backed off
    along the face. Long walks among turned solids measured it at most 0.00004 in, over 200 seeds
    of 20,000 frames at the first skin of 0.001, which a test holds at the three that were worst
    before.
  - A mover already within its radius of a face, an edge, or a corner, or just at it, touches it
    only going further in, so one starting in a seam goes out of it, never through, and one that
    rounding puts exactly its radius from an edge isn't held there. Edges' and corners' terms are
    worked out in double, since for a long sweep beside a big triangle they're far bigger than how
    near it passes.
  - A mover climbs ground, a slope within its max slope of level (45 degrees at first), as far as
    the slope goes, as physics' Character does. Sliding along anything steeper, edges and corners
    too, raises it no more than the move's own rise and its step height, so a tall mover isn't
    thrown up over a ledge, and a step up lands on ground no higher than its step height. A step
    height of 0 is ramps up to the max slope, and no steps.

## The renderer

- **No GPU, for a dedicated server** (2026-11-08): nothing loads Vulkan until something draws, makes
  a Texture, or shows a window, so World, entities, picks, scene files, physics, audio on MANUAL,
  Flip with no window, and an untextured Model all run with no driver, loader, or display. A Model
  makes its Textures as it loads, which would start the GPU, so a server calls
  `Gpu.SetDisabled(True)` (`fbe_gpu_set_disabled`) before anything else: Vulkan's instance and
  device are refused ("the GPU is disabled"), so a Model loads without its images (its materials
  keep their factors, TextureOf is Null), and a Texture or a draw throws. It's explicit, not a
  fallback when the GPU can't start, so a player's broken driver still says so, and a server on a
  machine with a GPU never makes a device. It's refused while the GPU's started. examples/
  dedicated_server runs so in ctest, with no Vulkan driver given.
- **Vulkan 1.2** with dynamic rendering, synchronization2, descriptor indexing (bindless
  textures), and buffer device addresses (shaders read buffers through addresses in push
  constants), through volk and VMA; MoltenVK on macOS supports that set. Two frames in flight.
  The window's images and the screen are sRGB, so drawing is in linear light. A camera can draw in
  stored values instead, off by default: textures read as stored, lit and blended as stored, into
  a UNORM target, with no tonemap, and an HDR camera refuses it. DirectX 7 blended stored values,
  so a game keeping that look (forced alpha 128 giving 128, not 188) needs it.
  A portability driver holds what it promises: on MoltenVK the bindless set is 1180 textures
  against 16384 elsewhere, and samplers take no LOD bias, which SetLodBias keeps and doesn't draw.
- **What it runs on** (the user, 2026-10-31): Vulkan 1.2 GPUs (2016 onward), Windows 10, Linux
  with glibc 2.28, macOS 13 on MoltenVK. **What it aims for** (the user, 2026-10-31): a scene of
  SCP:CB's scale in the high hundreds of fps, 500 or more at 1080p on the user's RTX 4080 and
  well over 144 on a GTX 1060-class GPU, so about 2 ms a frame for the engine, CPU and GPU: batched
  draws, culling, no per-object overhead per frame. Each milestone measures its test scenes
  against it.
- **Depth and clip space**: targets and the screen use reversed 32-bit float depth, cleared to 0,
  1 at the near plane, compared GREATER_OR_EQUAL, and Vulkan's clip space, y down. Cameras build
  their own projections for it; std.math's `Perspective` and `Frustum` are the general ones,
  near 0 and far 1, y up, and the engine doesn't use them for drawing.
- **A small frame graph**: each frame's passes (shadows, a prepass where one's asked for, opaque, sky,
  transparent, post, 2D overlay) declare what they read and write; the graph orders them, places
  barriers, and reuses transient targets.
- **Prepass**: only for a camera with ambient occlusion or antialiasing over frames: its main
  view's opaque draws in order 0 again, one sample, its own depth, into a half-float target: view
  depth and what 16 bits lose of it, and motion. It doesn't share depth with the scene pass: two
  shaders don't promise one position on every driver, and a test on AMD's shows they don't.
- **Ambient occlusion**: full resolution, 16 spiral samples, depth-aware blur both ways, normals
  from depth. It scales ambient and environment light only, of what's in the prepass; a tenth of
  the view's height is the most its samples reach. At 1080p, +0.26 ms on an RTX 4080, +9.2 ms on an
  AMD integrated GPU.
- **Antialiasing over frames**: on an HDR camera, Halton 2,3 of 8 in the scene pass's projection
  only; resolved before bloom, by motion at the nearest depth of 3 by 3, Catmull-Rom, clamped to 3
  by 3 in YCoCg, 0.1 of what's now once settled. What has no motion of its own: particles, what
  blends, other orders, morphs, an Instances' own instances, reflections. +0.14 ms and +6.7 ms.
- **FXAA**: after the tonemap, from the picture alone, +0.02 ms and +0.94 ms.
- **Extract, then draw**: after UpdateWorld, the scene is read into render lists (visible
  objects, sorted), so drawing never reads the live scene; a render thread can take drawing over
  later without changing the API.
- **PBR** is pbr.slang: color and layers its base color; metal-rough, normal, occlusion, and
  emissive maps read at run time, not by pipeline key; a surface's optional tangent stream, or a
  pixel's derivatives; a World's environment beside its ambient; a PBR base color is saturated.
- **A camera's HDR** is its own choice, off at first: an image of half floats at its target's
  size, then a full-screen pass into its target, so light past white is kept until then. All it
  draws goes through this, UNLIT, VERTEX, and its clear color too: white is 0.869 through NEUTRAL,
  0.673 through ACES, 0.5 through REINHARD, so what has to stay white wants NONE or a camera of its
  own, and an exposure of 0 clears to black. The order in that pass: light plus bloom, exposure,
  tonemap, color grade, gamma; a target stored as sRGB then encodes it. The grade is the ASC color
  decision list, slope, offset, power, then saturation, on linear values after the tonemap. For
  partly covered pixels the curves run on rgb over alpha, multiplied back after. At alpha 0, light
  is added over what's under it, graded by what that adds, so an offset lights no empty pixel and a
  non-clearing camera leaves what's under it alone.
- **Bloom**: what's past a threshold, fading in from half of it, down a chain from the camera's
  viewport to 8 to 15 texels on its short side, 13 taps down with Karis's average on the first
  level, a tent up. A level's worth is fixed from the coarsest, 0.8 of what's left by the one
  before, with the finest taking the rest, so the glow is at the same part of a view at any size.
  It's added before exposure, and over partly covered pixels as a layer of its own. A viewport
  under 16 pixels on its short side has none.
- **One material system, three shading models**: PBR, UNLIT, and VERTEX (per-vertex lighting,
  for a retro look). Texture layers with blend modes and their own UV sets, lightmaps (SCP:CB's
  rooms are lightmapped), fog, and alpha all go through the same pipelines, as specialization
  constants, so any mix of them draws in the same pass.
  UNLIT and VERTEX are the fixed-function look, so they work in encoded values: layers, ADD, ×2,
  and fog on what's stored, with only the final color converted to linear for the target; PBR is
  linear throughout. VERTEX's light is `min(range / distance, 1)` with no end, clamped per vertex
  before texturing, from the first 8 shown lights in the tree's order; PBR keeps
  KHR_lights_punctual's window, which clusters need. Range means each in its model. A Texture has
  an alpha flag, which AUTO blending honors. A Texture's mipmaps are made from its first level by
  the GPU, or a game writes a level of its own (`WriteLevel`), which leaves the others as they are,
  as for mipmaps that keep what an alpha cutoff keeps; a write of the first level makes them all
  again (2026-11-05).
- **Lights**: clustered forward, so the many small lights of a facility work, per pixel or per
  vertex. A point or spot light ends at its range, as clustering needs, and falls off inside it as
  glTF's KHR_lights_punctual says, the inverse square windowed to 0 at the range, in every shading
  model. PBR draws take the clusters' lights; VERTEX draws take lists of their own, the lights
  that reach them, at most 8 in the tree's order, so a light by an off-screen vertex still lights
  it. A mirrored world matrix is a pipeline key bit of its own, as the front face is baked into a
  pipeline. Shadows come with E6.
- **Order and depth**: material orders other than 0 neither test nor write depth and draw
  furthest first, behind the scene or over it, as Blitz's EntityOrder did; in order 0, opaque
  draws test and write depth, by pipeline, material, and nearest first, and what blends only
  tests it, furthest first. Each draw carries its pipeline key (shading, blend, alpha cutoff,
  vertex colors, sides, fog, depth), which a texture layer's part joins when textures land.
- **Render to texture**: any Camera can render into a Texture (security monitors, mirrors,
  portals), and post effects run per camera.
- **2D** draws after 3D, in the same frame graph, into the window or into a Texture; pixel
  access (`Lock`, `ReadPixel`, `WritePixel`) works on a CPU copy, uploaded or read back once per
  frame, never per pixel.
- **Text** is FreeType's rasterizing and HarfBuzz's shaping, statically linked, into glyph
  atlases (the user, 2026-10-31).
- **Names**: fbengine's GPU image is `Texture`, made from a file or a std.image `Image`, and its
  `Font` lives in its 2D module, qualified where a file imports both (the user, 2026-10-31).
  A 2D drawable is a `Sprite2D`, which a Canvas draws; `Sprite` is the scene's billboard entity,
  as Mesh and Camera are entities (2026-10-31, as Godot names its Sprite2D and Sprite3D). A
  Canvas's blend is `Blend2D`; a Material's is `Blend`.
- **Shaders** are Slang, compiled to SPIR-V when the engine's built and embedded in the DLL
  (the user, 2026-10-31).
  Games' own shaders (E6) come with the asset pipeline's build step.
  A shader of its own sees, as it moves a vertex, where that vertex's instance is in its Instances'
  space (`MovingVertex.place`, 0 for what isn't an instance), so each of a field of instances moves
  on its own beat with no parameter of its own; an Instances culls each instance by its geometry's
  cull box, as a Mesh does, for what its shader moves out of its bounds (2026-11-05).

## Assets

- Paths are std.vfs's (`res://`), with folders in development and pak files in a release.
- Loaders are C++ (fastgltf, KTX, stb_image), on worker threads.
  `Assets.Load<T>(path)` loads now; `Await Assets.LoadAsync<T>(path)` loads on a worker, and the
  Task completes on the main thread at a Flip, or at a tick of the main scheduler while FB waits.
- A cache keyed by normalized path hands out the same asset again, refcounted; `Free()` and the
  last reference let it go.
- A game's `[build]` names fbengine_tools' step, `step/game.fb`. Its files are in `assets/`, and
  each build the step writes its module `res`, the typed references to them, with a `Mount`: a
  debug build mounts `assets/` and watches it for hot reload, and a release cooks it into
  `build/<name>.pak`, each file again only when it or its settings change, and mounts the pak
  beside the executable or in a macOS bundle's Resources. The cooker's written paks are stored
  zips, a file at a time, with Zip64 where sizes or counts need it. Names starting with '.' aren't
  in a folder mount, a pak, or the references, and Thumbs.db and desktop.ini aren't in the last
  two.
- The engine loads modern formats only: glTF for meshes, scenes, and animation; KTX2, PNG, and
  JPEG for textures; OGG, WAV, and FLAC for sound. Converting anything else is a game's own
  tooling, outside the engine and the language (the user, 2026-10-31).

## Threads

- The API is the main thread's (`@MainThread`). Inside, the C++ core has one job system, a
  worker per core less one, which asset decoding, culling, animation, and collisions use;
  Jolt runs on its own pool.
- Nothing calls into FB from a worker; finished work is queued and handed over at Flip, and at
  the main scheduler's ticks while FB waits without one.

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
