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
original 1.3.11, and ue5-additions.md what v5 adds; the steps below cover both.

## Milestones

| | Goal | Needs | Done when |
|---|---|---|---|
| **E0** | Setup | | done (2026-10-31) |
| **E1** | App and input | E0 | done (2026-10-31) |
| **E2** | The GPU core | E1 step 1 | golden images of a triangle, a textured quad, and a readback match on lavapipe |
| **E3** | 2D | E2 | a small arcade game runs, and its frames match their golden images |
| **E4** | The scene | E1 step 2 | done (2026-10-31) |
| **E5** | Drawing the scene | E2, E4 | done (2026-11-02): a lightmapped, fogged, lit room with cameras rendering to textures matches its golden images |
| **E6** | The modern renderer | E5 | Khronos's glTF sample models match their references, with shadows and post effects |
| **E7** | Animation | E4 (E5 to draw it) | skinned and morphing sample models match their reference frames |
| **E8** | Assets | E2, E4; a FutureBlitz build step for its last steps | a game runs from cooked paks, and reloads changed files in development |
| **E9** | Audio (fbengine_audio) | E1 step 2 | done (2026-10-31; sounds following entities, 2026-11-02): sounds follow entities |
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
7. `ShowMessage(title, text)`, the OS's message box, for errors players should see; `OpenURL`,
   which opens a link or a file as the OS does; loose ends; E1's review.

Steps 3 to 7 done (2026-10-31, landed at 3698cf3).

### E2: The GPU core

1. Vulkan through volk and VMA: instance, device (preferring a discrete GPU, lavapipe in tests),
   queues, the window's surface and swapchain, two frames in flight; `Flip` presents, with vsync
   or without; resizing and minimizing.
2. Shaders: Slang compiled to SPIR-V at build time and embedded; pipeline creation and a cache on
   disk; the golden image harness (render on lavapipe, compare with a checked-in PNG within a
   tolerance, a tool to rewrite them).
3. Buffers and textures: uploads through staging, mipmaps, samplers (filtering, wrap, LOD bias,
   anisotropy), bindless descriptors; formats sRGB and linear.
4. Render targets and readback: offscreen color and depth targets (depth reversed, 32-bit
   float, for near planes as close as 0.01), MSAA and resolve, copying a target into a texture,
   reading pixels back to the CPU, saving a frame as a PNG (screenshots); a render size apart from
   the window's, scaled to it.
5. The frame graph: passes with declared reads and writes, barriers, transient targets reused.
6. Loose ends and E2's review: device loss, a GPU that lacks a feature (a clear error), MoltenVK
   on macOS, timings, and counters (draws, triangles) for a debug overlay.

Steps 1 to 6 done (2026-11-01, landed at e9d02d6, the gpu lane's merge).

### E3: 2D

1. The 2D canvas: drawing into the window or a target, origin, viewport (clip), color, lines,
   rectangles, ovals, polygons, batched into few draws.
2. Textures as images: load, draw with handle (pivot), rotation, scale, color and alpha, frames
   of a sheet, tiles, regions; drawing into a texture.
3. Text: TrueType fonts from files and the system's, by name, size, and style; glyph atlases;
   measuring; text drawn crisp at any DPI. Its core, on the CPU (FreeType and HarfBuzz, fonts,
   fallback, shaping, measuring, and the atlas's pages and glyph places), is the input lane's,
   in `native/fbengine/text/`, so E11 doesn't wait; the gpu lane uploads the pages and draws.
4. Pixels: locking a texture or the window for CPU reads and writes, through one copy per frame;
   copying regions between the window, textures, and images; std.image's Image in and out.
5. Collisions in 2D: rectangles, and pixel-exact masks of images (the input lane's, on the CPU).
6. Loose ends and E3's review: the arcade game example and its golden images.

Steps 1 to 5 done (2026-11-01: 1, 2, 4, and 3's drawing landed at e9d02d6; 3's core and 5 at
0d0c099).

### E4: The scene

1. Worlds and entities: the tree, names, find, children in order, show and hide, copy (with
   children, sharing geometry), Free, `SetData`; the handle tables in C++ and FB.
2. Transforms: position, rotation (Quat and Euler), scale, local and world, move, turn,
   translate, point at, align to a vector, transforming points and vectors between entities,
   angles to a point; the dirty-flag update.
3. Cameras: perspective and orthographic, field of view (vertical, the horizontal following
   the aspect ratio), near and far, viewport, clear modes and color, render order, several a frame;
   projecting points to the screen and back.
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
10. Sprites and billboards as entities; `UpdateWorld(seconds)` and `FixedUpdate(rate)` with
    tweening.
11. Loose ends and E4's review: ground planes, mirrors, fading by distance, meshes crossing.
12. Terrain: a heightmap entity made in code or from an image (E8's loading), its height and
    normal at a point, collisions and picks against its grid, changed in place; its LOD drawing
    is E5's.

Steps 1 to 12 done (2026-10-31, landed at a74f435), with E5 step 1's CPU half: what cameras
draw, and the projection they draw with.

### E5: Drawing the scene

1. Extract: visible entities into render lists, sorted by material and depth; `RenderWorld()`
   draws every camera.
2. Materials on the GPU: one pipeline family, specialization constants for layers, blends, UV
   sets, vertex colors, lightmaps, fog, alpha test; a bump-offset layer, whose map shifts the next
   layer's UVs (the environment bump SCP:CB's rooms use); the UNLIT and VERTEX shading models.
3. Lights: clustered forward, per pixel or per vertex, many small lights; point lights' falloff,
   spot lights' inner and outer cones; the ambient light.
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

0. The job system, which scene's collisions and assets' loading both run on: a worker per core
   less one, jobs and their dependencies, and finished work handed to the main thread at Flip.
1. The VFS (`res://`), folders and pak files, mounted over each other.
2. Loading: glTF (meshes, materials, scenes, skins, clips), KTX2, PNG, JPEG, OGG, WAV, FLAC, on
   worker threads; `Assets.Load<T>` and `Await Assets.LoadAsync<T>` with progress.
3. The cache: one asset per path, refcounted; import settings in sidecar files.
4. The cooker: compressed textures (BC7, ASTC in KTX2), optimized meshes (meshoptimizer), paks.
5. Typed references and hot reload, through FutureBlitz's package build step.
6. Loose ends and E8's review.

Steps 0 to 3, and 4 and 5 but for running in `fb build`, done (2026-10-31, landed at d4c0dbd).

### E9: Audio (fbengine_audio)

1. miniaudio in fbengine_audio_native; the device, the null backend for tests.
2. Sounds and channels: load, play, loop, volume, pan, pitch, pause, resume, stop, playing; a
   finished channel ignores calls.
3. Streams: music and long sounds from OGG and FLAC, looping, crossfades.
4. 3D sound: a listener and emitters following entities (through Frame's hooks), rolloff,
   Doppler.
5. Buses and effects: groups with volume, low-pass, reverb.
6. Loose ends and E9's review.

Steps 1 to 6 done (2026-10-31, landed at 5d7e461), and step 4's entity overloads (2026-11-01, at
e9d02d6).

### E10: Physics (fbengine_physics)

1. Jolt in fbengine_physics_native; worlds, stepping, its job system (done, 2026-10-31).
2. (Done, 2026-10-31.) Bodies and shapes: static, dynamic, kinematic; boxes, spheres, capsules, convex hulls, meshes
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

1. MPEG-1 through pl_mpeg in fbengine_video_native, behind a decoder interface another format
   can join, decoding on a worker thread.
2. Playing into a texture, with its sound through fbengine_audio, in sync, or beside a sound
   stream of its own when the video has none; letterboxing; pause, seek, loop; knowing when it
   ends.
3. Loose ends and E12's review.

### E13: The editor

A scene and asset editor, a FutureBlitz program on the packages and fbengine_ui: a scene view,
the tree, an inspector, an asset browser, play in the editor. Code stays in VS Code and fb-lsp.
It's a package, fbengine_editor, whose program opens a game's project folder.

1. The shell: panes on fbengine_ui that dock and resize, a project opened by its folder and
   mounted, what's open kept between runs, every action a command with undo and redo.
2. The scene view: a camera rendering into a pane's texture, flown and orbited, a grid, selection
   by a pick, handles to move, turn, and scale in world or local space, snapping.
3. The tree: a scene's entities, renamed, reparented by dragging, copied, hidden, freed, found by
   name, each through undo.
4. The inspector: each thing's properties by FutureBlitz's reflection, what has both an X() and a
   SetX(), with editors by type, and what each setter takes by attributes on it (`@Limits`,
   `@ShownInDegrees`, `@NotEdited`); materials, lights, cameras, bodies, and a game's own components,
   which are data: a game's small program, its components/main.fb, says what kinds it has, and
   fbengine_physics adds "Body" the same way.
5. The asset browser: res:// as a tree with thumbnails, a model dragged into the scene, import
   settings' sidecars edited, what changes on disk reloaded.
6. Scenes on disk: a text format diffs and merges well, written whole or not at all, with
   references by res:// path; `Scene.Load` in the engine for a game to read what's saved.
7. Play in the editor: the game's own program built by fb and run on the saved scene, stopped from
   the editor, its output shown; the scene's never changed by a run.
8. Loose ends and E13's review: every action driven headless by Simulate, each pane's look in
   golden images, and a `--watch`.

### E14: The proof

A sample for each engine requirement in docs/research/scpcb-requirements.md's section 6, each
built from fbengine's own features (lightmapped and fogged rooms, cameras into textures, several
cameras a frame, swept collisions with sliding, picks and in-view tests, meshes built in code with
two UV sets, skinned animation with bones under game control, streamed and positional sound,
video, a console), so every feature the remake will need is known to work; and a modern
showcase game. The remake itself is a separate, later project.

1. The samples, under examples/proof/, whose README maps each folder to its items. Each proves its
   item's named things with numbers worked out apart from the code, on all three systems; what a
   game owns (Blitz's random generator, the .rmesh reader, a map from a seed) is the sample's own
   code, never the engine's.
2. A camera that draws in stored values, off by default, as DirectX 7 blended them: what items 7,
   12, and 27 mean by the look Blitz had.
3. A real display's modes, in a test only CI runs, as nothing may open a window on the user's PC.
4. Slack Water, the showcase, in examples/slack_water and docs/slack-water.md.

## Lanes

Lanes work as docs/lanes.md describes, as FutureBlitz's did in M6: each a Claude Code session in a
worktree of this repository, on a branch of its own, building one milestone's steps in order;
the coordinator (the session that starts them) reviews each step and lands it on master, and
alone edits this file, docs/architecture.md, docs/questions.md, CLAUDE.md, and FutureBlitz's
docs.

**Before the lanes** (done, 2026-10-31): the C API split by area (`native/fbengine/<area>/`,
`fbe_<area>.h`, one binding file each, by `tools/bindgen.sh`); `tools/verify.py`, every lane's
and every landing's check; every package's tests and exports checked by ctest and CI without
naming it; `publish.yml` publishing every package; and `Frame.OnFlip`, the hook packages use to
run each frame.

**The lanes**: six at once, each going on to its next milestone when its last one lands. A lane
that waits on another lane's step builds what it can without it, and the rest once it lands.

| Lane | Builds, in order | Waits on | Owns |
|---|---|---|---|
| input | E1 steps 3 to 7, then E11, with E3 step 3's text core and E3 step 5 | E11's drawing: gpu's E3 steps 1 to 3 | `native/fbengine/input/`, `text/`, the input and text modules, Simulate's input |
| gpu | E2, then E3, then E6 | E6: scene's E5 | `native/fbengine/gpu/`, `2d/`, `shaders/`; the window's Vulkan surface in `window/`; Flip's present in `frame/` |
| scene | E4, then E5 | E5: gpu's E2; E4 step 8: assets' job system | `native/fbengine/scene/` (then `render/`), the scene modules |
| assets | E8 step 0, the job system; E8 steps 1 to 3; then 4 and 5 | step 2's textures: gpu's E2 step 3; meshes: scene's E4 step 5; steps 4 and 5: FutureBlitz 1.1's build step | `native/fbengine/jobs/`, `assets/`, the asset modules; fbengine_tools |
| audio | E9, then E7 | E9 step 4: scene's E4 step 2; E7: scene's E4 (E5 to draw it) | `native/fbengine_audio/`, `packages/fbengine_audio/`, then `native/fbengine/anim/` and the animation modules |
| physics | E10, then E12, then E13 | E10 step 2's meshes: scene's E4 step 5; step 3: scene's E4 step 2; E12 step 2: gpu's E3 step 2; E13: E5, E7, E8, E11 | `native/fbengine_physics/`, `packages/fbengine_physics/`, then fbengine_video's, then the editor |

E8 step 0, the job system, comes first in the assets lane, since scene's collisions and assets'
loading both run on it: a worker per core less one, jobs and their dependencies, and finished work
handed to the main thread at Flip. E14 comes last, after every lane's milestones.

**After E5** (2026-11-02): E6 goes to two lanes, so its steps don't wait on one: gpu takes steps 1, 3,
4, 7, and 8 (PBR, HDR, post effects, materials' own shaders, the review), and scene takes 2, 5, and 6
(shadows; GPU particles, decals, and sky; instancing, LOD, and occlusion culling). audio finishes E7
by drawing what assets' Model loads. physics builds E13. input, then whichever lane's free, builds
E14's samples under examples/proof/.

**Shared files**: the top CMakeLists.txt, `.github/workflows/`, `tests/CMakeLists.txt`, and
`native/fbengine/frame/frame.cpp` take additions at the end of a list, one call per lane in
Flip's order (events to input, then the present), so landings merge cleanly. Anything else
outside a lane's own area goes through the coordinator.

**FutureBlitz**: what a lane finds in the language, fb, or std goes to the coordinator with a
program that shows it, and is fixed there, in a FutureBlitz release, never worked around here.
A FutureBlitz lane builds what the engine's lanes found, released as FutureBlitz 1.1
(the user, 2026-10-31): dependencies with both a path and a version (so fbengine_audio and
fbengine_physics depend on fbengine and still publish), a built-in `Object` (for `SetData` and
`Data<T>`), package tests seeing Package members (so test aids stay Package), and a package
build step, a FutureBlitz program named in project.toml that `fb build` runs before compiling,
whose output joins the build (E8's cooker and typed references). Every change lands with its
docs, the website's included.

**Each step**: the lane builds it, `tools/verify.py` passes, it pushes and reports; the
coordinator reviews it (one review per step, about three reviewers and a skeptic each, as M6's),
the lane fixes every confirmed finding with a test, and the coordinator lands it after verify
passes on the branch rebased onto master, then CI. Nothing a review finds is left for later.
