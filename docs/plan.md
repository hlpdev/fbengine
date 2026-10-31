# fbengine's plan

The milestones, their steps, what each needs first, and how lanes build them. docs/architecture.md
has how the engine fits together; docs/questions.md what waits on the user; docs/research/ the
read-throughs of Blitz3D and SCP:CB this plan comes from. FutureBlitz's docs/engine.md keeps the
summary table.

fbengine is its own engine, not a modern copy of Blitz3D's (the user, 2026-10-31). Its feature
set has to cover every engine feature SCP: Containment Breach Ultimate Edition v5 needs (the
same game as SCP: Containment Breach Remastered), checked at E14, so a remake indistinguishable to
play can be built on it later. The remake is a separate project, much later, with a codebase of
its own, clean and nothing like the original's, and its own tools and converters for the game's
assets, tied to neither the engine nor the language. docs/research/scpcb-requirements.md read the
original 1.3.11; Ultimate Edition v5's additions are read when its source is in hand.

## Milestones

| | Goal | Needs | Done when |
|---|---|---|---|
| **E0** | Setup | | done (2026-10-31) |
| **E1** | App and input | E0 | a test under the offscreen driver reads every kind of input, and coroutines resume once per frame |
| **E2** | The GPU core | E1 step 1 | golden images of a triangle, a textured quad, and a readback match on lavapipe |
| **E3** | 2D | E2 | a small arcade game runs, and its frames match their golden images |
| **E4** | The scene | E1 step 2 | transforms, hierarchy, visibility, collisions, and picks pass their tests, with no GPU |
| **E5** | Drawing the scene | E2, E4 | a lightmapped, fogged, lit room with cameras rendering to textures matches its golden images |
| **E6** | The modern renderer | E5 | Khronos's glTF sample models match their references, with shadows and post effects |
| **E7** | Animation | E4 (E5 to draw it) | skinned and morphing sample models match their reference frames |
| **E8** | Assets | E2, E4; a FutureBlitz build step for its last steps | a game runs from cooked paks, and reloads changed files in development |
| **E9** | Audio (fbengine_audio) | E1 step 2 | on miniaudio's null backend, tests mix to known buffers, and sounds follow entities |
| **E10** | Physics (fbengine_physics) | E1 step 2 | Jolt's sample scenes stay stable, deterministic runs repeat, and bodies drive entities |
| **E11** | UI (fbengine_ui) | E3 | a sample game's menus, HUD, and console work by mouse, keyboard, and gamepad |
| **E12** | Video (fbengine_video) | E3, E9 | a video plays into a texture with its sound in sync |
| **E13** | The editor | E5, E7, E8, E11 | a scene is made, saved, played, and inspected in the editor |
| **E14** | The proof | everything | a sample for each of SCP:CB's engine requirements runs, and a modern showcase game, on Windows, Linux, and macOS |

The order below is one possible: with lanes, E2, E4, E9, and E10 start together once E1's input
is in, and the rest start as what they need lands.

## Steps

Each step lands with tests, as FutureBlitz's do, and with its API in fb doc comments.

### E1: App and input

1. SDL3 and the window (done).
2. Flip, frame timing, the window's events, Simulate (done).
3. The keyboard: `Key` by position, `KeyDown`, `KeyHit`, `KeyUp` counted per frame, `FlushKeys`;
   text input with IME (`TextInput()`, `StartTextInput`, `StopTextInput`); Simulate's keys and
   text.
4. The mouse: position in window units, wheel, speeds since the last frame, buttons (Down, Hit,
   Up), `MoveMouse`, cursor visible, relative mode for mouse look with raw motion; Simulate's
   mouse.
5. Gamepads and joysticks: SDL3's gamepads (buttons, axes past a dead zone, rumble, connecting),
   and raw joysticks for wheels and sticks; tests on SDL's virtual devices.
6. Input actions: actions bound to keys, buttons, and axes, with Down, Hit, Up, and Value;
   two-axis actions; bindings saved and loaded as TOML; rebinding from the next input pressed.
7. `ShowMessage(title, text)`, the OS's message box, for errors players should see; loose ends;
   E1's review.

### E2: The GPU core

1. Vulkan through volk and VMA: instance, device (preferring a discrete GPU, lavapipe in tests),
   queues, the window's surface and swapchain, two frames in flight; `Flip` presents, with vsync
   or without; resizing and minimizing.
2. Shaders: Slang compiled to SPIR-V at build time and embedded; pipeline creation and a cache on
   disk; the golden image harness (render on lavapipe, compare with a checked-in PNG within a
   tolerance, a tool to rewrite them).
3. Buffers and textures: uploads through staging, mipmaps, samplers (filtering, wrap, LOD bias,
   anisotropy), bindless descriptors; formats sRGB and linear.
4. Render targets and readback: offscreen color and depth targets, MSAA and resolve, copying a
   target into a texture, reading pixels back to the CPU; a render size apart from the window's,
   scaled to it.
5. The frame graph: passes with declared reads and writes, barriers, transient targets reused.
6. Loose ends and E2's review: device loss, a GPU that lacks a feature (a clear error), MoltenVK
   on macOS, timings.

### E3: 2D

1. The 2D canvas: drawing into the window or a target, origin, viewport (clip), color, lines,
   rectangles, ovals, polygons, batched into few draws.
2. Textures as images: load, draw with handle (pivot), rotation, scale, color and alpha, frames
   of a sheet, tiles, regions; drawing into a texture.
3. Text: TrueType fonts from files and the system's, by name, size, and style; glyph atlases;
   measuring; text drawn crisp at any DPI.
4. Pixels: locking a texture or the window for CPU reads and writes, through one copy per frame;
   copying regions between the window, textures, and images; std.image's Image in and out.
5. Collisions in 2D: rectangles, and pixel-exact masks of images.
6. Loose ends and E3's review: the arcade game example and its golden images.

### E4: The scene

1. Worlds and entities: the tree, names, find, children in order, show and hide, copy (with
   children, sharing geometry), Free, `SetData`; the handle tables in C++ and FB.
2. Transforms: position, rotation (Quat and Euler), scale, local and world, move, turn,
   translate, point at, align to a vector, transforming points and vectors between entities,
   angles to a point; the dirty-flag update.
3. Cameras: perspective and orthographic, field of view, near and far, viewport, clear modes and
   color, render order, several a frame; projecting points to the screen and back.
4. Lights and fog as data: directional, point, spot, range, color, the ambient light; fog per
   camera.
5. Geometry: meshes built in code (surfaces, vertices with two UV sets, colors and alpha,
   triangles, normals), transformed, merged, copied, flipped; bounds; primitive shapes (cube,
   sphere, cylinder, cone, plane).
6. Materials as data: color, alpha, shininess, texture layers with blend modes and UV sets,
   lightmaps, vertex colors, fullbright, two-sided, fog on or off, order and blending; shared by
   entities, cloned on write.
7. Visibility: frustum tests of bounds and points (in view), distance, line of sight against
   geometry.
8. Collisions: swept spheres and ellipsoids against meshes, boxes, and spheres; stop and slide
   responses, step-up, several contacts per update, by collision groups; results per entity.
9. Picks: rays and spheres against entities and geometry, with pick modes, and the hit's
   position, normal, entity, and triangle.
10. Sprites and billboards as entities; `UpdateWorld(seconds)` and `FixedStep(rate)` with
    tweening.
11. Loose ends and E4's review.

### E5: Drawing the scene

1. Extract: visible entities into render lists, sorted by material and depth; `RenderWorld()`
   draws every camera.
2. Materials on the GPU: one pipeline family, specialization constants for layers, blends, UV
   sets, vertex colors, lightmaps, fog, alpha test; the UNLIT and VERTEX shading models.
3. Lights: clustered forward, per pixel or per vertex, many small lights; the ambient light.
4. Transparency and order: blending modes, sorting far to near, order groups drawn first or last.
5. Cameras into textures: a camera rendering into a texture, several cameras and viewports a
   frame, orthographic overlays; copying the frame into a texture (feedback effects).
6. Sprites, billboards, and particles drawn in bulk (the particle system is game code over
   sprites, or E6's GPU particles).
7. Loose ends and E5's review: a lightmapped, fogged room test scene.

### E6: The modern renderer

1. PBR (glTF's metal-rough model), normal maps, emissive; image-based lighting.
2. Shadows: cascaded for directional lights, spot and point shadows.
3. HDR targets, exposure, tonemapping, gamma.
4. Post effects: bloom, SSAO, FXAA, TAA, MSAA, color grading, a game's own full-screen passes.
5. GPU particles, decals, sky and atmosphere.
6. Instancing, LOD, occlusion culling.
7. Materials with their own shaders, compiled by the asset pipeline.
8. Loose ends and E6's review: Khronos's sample models.

### E7: Animation

1. Skeletons: bones, bind poses, sampling clips by time or frame, skinning on the GPU.
2. Clips and blending: play, loop, ping-pong, speed, crossfades, layers, events at frames.
3. Bones under game control after animation (turning a head toward a target).
4. Morph targets; node animation of whole entities.
5. Loose ends and E7's review.

### E8: Assets

1. The VFS (`res://`), folders and pak files, mounted over each other.
2. Loading: glTF (meshes, materials, scenes, skins, clips), KTX2, PNG, JPEG, OGG, WAV, FLAC, on
   worker threads; `Assets.Load<T>` and `Await Assets.LoadAsync<T>` with progress.
3. The cache: one asset per path, refcounted; import settings in sidecar files.
4. The cooker: compressed textures (BC7, ASTC in KTX2), optimized meshes (meshoptimizer), paks.
5. Typed references and hot reload, through FutureBlitz's package build step.
6. Loose ends and E8's review.

### E9: Audio (fbengine_audio)

1. miniaudio in fbengine_audio_native; the device, the null backend for tests.
2. Sounds and channels: load, play, loop, volume, pan, pitch, pause, resume, stop, playing; a
   finished channel ignores calls.
3. Streams: music and long sounds from OGG and FLAC, looping, crossfades.
4. 3D sound: a listener and emitters following entities (through Frame's hooks), rolloff,
   Doppler.
5. Buses and effects: groups with volume, low-pass, reverb.
6. Loose ends and E9's review.

### E10: Physics (fbengine_physics)

1. Jolt in fbengine_physics_native; worlds, stepping, its job system.
2. Bodies and shapes: static, dynamic, kinematic; boxes, spheres, capsules, convex hulls, meshes
   (from fbengine's geometry).
3. Bodies driving entities and entities driving kinematic bodies, synced in a batch a frame.
4. Joints, the character controller, triggers, ray and shape casts, layers.
5. Deterministic stepping; loose ends and E10's review.

### E11: UI (fbengine_ui)

1. Layout (rows, columns, stacks, anchors) and themes, on E3's canvas and text.
2. Widgets: label, button, toggle, slider, text field, list, scroll view, image, progress.
3. Focus and navigation by keyboard and gamepad; input routed between UI and game.
4. A debug UI and a console.
5. Localization hooks; loose ends and E11's review.

### E12: Video (fbengine_video)

1. A decoder (docs/questions.md) in fbengine_video_native, decoding on a worker thread.
2. Playing into a texture, with its sound through fbengine_audio, in sync; pause, seek, loop.
3. Loose ends and E12's review.

### E13: The editor

Designed when E5, E7, E8, and E11 land; its scope is a question for the user. It's a FutureBlitz
program on the packages: a scene view, the tree, an inspector, an asset browser, play in the
editor.

### E14: The proof

A sample for each engine requirement in docs/research/scpcb-requirements.md's section 6, each
built from fbengine's own features (lightmapped and fogged rooms, cameras into textures, several
cameras a frame, swept collisions with sliding, picks and in-view tests, meshes built in code with
two UV sets, skinned animation with bones under game control, streamed and positional sound,
video, a console), so every feature the remake will need is known to work; and a modern
showcase game. Planned when E13 lands. The remake itself is a separate, later project.

## Lanes

Lanes work as FutureBlitz's did in M6 (its docs/m6-plan.md): each a Claude Code session in a
worktree of this repository, on a branch of its own, building one milestone's steps in order;
the coordinator (the session that starts them) reviews each step and lands it on master, and
alone edits this file, docs/architecture.md, docs/questions.md, CLAUDE.md, and FutureBlitz's
docs.

**Before the lanes**, one step by the coordinator splits what every lane would otherwise share:

- The C API header by area (`fbe_core.h`, `fbe_window.h`, `fbe_frame.h`, `fbe_input.h`,
  `fbe_gpu.h`, `fbe_scene.h`, ...) under one `fbengine.h`, and one binding file per area in
  `c/`, made by `tools/bindgen.sh`.
- `native/fbengine/<area>/` folders, with CMake taking each folder's sources.
- `tools/verify.py`: the build, ctest, the package tests, the export check, clang-format,
  clang-tidy on what changed, and `fb fmt --check`; every lane's and every landing's check.

**The first wave**, five lanes:

| Lane | Builds | Owns |
|---|---|---|
| input | E1 steps 3 to 7 | `native/fbengine/input/`, `fbe_input.h`, packages/fbengine's input modules |
| gpu | E2, then E3 | `native/fbengine/gpu/` (then `2d/`), `shaders/` |
| scene | E4 | `native/fbengine/scene/`, `fbe_scene.h`, the scene modules |
| audio | E9 | `native/fbengine_audio/`, `packages/fbengine_audio/` |
| physics | E10 | `native/fbengine_physics/`, `packages/fbengine_physics/` |

**Later waves**, as lanes finish: E5 (once E2 and E4 land), E7, E8, E11, E12, E6, then E13. A
FutureBlitz lane builds the package build step E8 needs, once the user settles its design.

**Each step**: the lane builds it, `tools/verify.py` passes, it pushes and reports; the
coordinator reviews it (one review per step, about three reviewers and a skeptic each, as M6's),
the lane fixes every confirmed finding with a test, and the coordinator lands it after verify
passes on the branch rebased onto master, then CI. Nothing a review finds is left for later.
