# The proof

A sample for each engine requirement in [section 6](../../docs/research/scpcb-requirements.md#6-engine-requirements)
of SCP:CB's requirements, as [E14](../../docs/plan.md#e14-the-proof) asks: each proves its item's named things with
numbers worked out apart from its code, and what a game owns (Blitz's random generator, the room file's reader, a
map from a seed, a converter) is the sample's own code, never the engine's.

Each prints what it found, line by line, and `ctest -R proof` checks those lines against the numbers in
[tests/CMakeLists.txt](../../tests/CMakeLists.txt). With `--watch`, each opens a window to look at; the tests walk
that offscreen for 40 frames, and once more to see that Escape and closing the window end it. One that needs a GPU
says so on a machine with none, and the test is skipped.

```
fb run examples/proof/picks
fb run examples/proof/picks -- --watch
```

| # | Requirement | Folder | What it shows |
|---|---|---|---|
| 1 | A Blitz-compatible random generator | [seeded_random](seeded_random) | Blitz's Park-Miller: the same seed, the same sequence; its 10000th from a seed of 1 is 399268537, Park and Miller's own check, worked out by the multiplier alone. The game's own code, in park_miller.fb |
| 2 | Blitz's collisions | [swept_collisions](swept_collisions) | Swept ellipsoids and balls against meshes, boxes, and other movers, stopped or slid whatever their step: falling, into a wall and a corner, up a step, with what each touched. Where fbengine differs on purpose, each is pinned |
| 3 | Picks and visibility | [picks](picks), [in_view_by_bounds](in_view_by_bounds) | A line, a line with a radius, what's ahead of an entity, and under a point of a camera's view, with what each reports; line of sight, and what blocks it, in in_view_by_bounds |
| 4 | `EntityInView` exactly as Blitz | [in_view_by_bounds](in_view_by_bounds) | An entity by its bounds' 8 corners, a point, and a box, against the camera's 6 planes, near and far included, nothing hiding anything |
| 5 | The scene | [scene_trees](scene_trees) | Parents and children, what's inherited, names, children in the order they were made, copying a tree, hiding and showing, and every way of moving. Blitz's degrees and left-handed axes are a recorded difference |
| 6 | Building meshes in code | [meshes_in_code](meshes_in_code) | Surfaces with two UV sets, colors and alpha, normals from triangles; read back, changed, joined, cloned, and moved whole |
| 7 | The fixed-function look | [fixed_function_look](fixed_function_look) | Layers by blend and UV set, as (2 x ambient x vertex + lightmap) x diffuse, each stage clamped; vertex lighting by range over distance, ambient, and 8 lights at most; linear fog; EntityFX's flags; blend from a texture's alpha; orders and sorting; sprites by each view mode. In linear light, and in stored values as DirectX 7 blended |
| 8 | The room file format | [room_files](room_files) | A room file written by hand and read back by the sample's own reader, room_file.fb: two UV sets and layers by exact pixels, collision by a mover, picks, and files that aren't one |
| 9 | Window, frame timing, and input | [frame_timing_and_input](frame_timing_and_input) | A millisecond clock, Flip with and without vsync, a frame limit, keys down, hit, and flushed, the mouse's place, speed, buttons, and wheel, moving and hiding it, relative mode, and focus. A game's own: its DirectInput scancode table and its mouse scaling |
| 10 | Several cameras in one frame | [cameras_and_viewports](cameras_and_viewports) | Viewports of one target each clearing its own, in order; an orthographic overlay; several worlds in a frame; field of view, range, and fog each a camera's own; hiding a camera |
| 11 | 2D over 3D | [sprites_over_a_scene](sprites_over_a_scene) | Images with either key color over a rendered mesh, whole, by regions, by frames of a strip, resized, turned, scaled, with a handle; shapes; text in the engine's font and a TrueType file's; overlaps by rectangles and masks |
| 12 | Render to texture and screen copies | [camera_into_a_texture](camera_into_a_texture) | A camera's target on a mesh another camera sees, as a security monitor; the frame copied into a texture and drawn back, as a blur; drawing and clearing into a texture; copying part of one to another; all of it in stored values, as DirectX 7 did |
| 13 | Skinned animation, bones under game control | [skinned_animation_with_bones_under_game_control](skinned_animation_with_bones_under_game_control) | A figure posed at a fraction of a frame, crossfaded, then its head turned by the game after each update and its arm released for good |
| 14 | Converting .b3d and .x to glTF | [models_as_a_converter_writes_them](models_as_a_converter_writes_them) | The receiving side of a converter: node and bone names as they were, frames by its clip's rate, a surface's material and its texture's name, and vertex colors exactly. The converter's a game's own |
| 15 | Audio as Blitz had it | [streamed_and_positional_sound](streamed_and_positional_sound) | Ogg Vorbis and sounds made in code, channels with volume, pan, and pitch, pause and resume, 64 mixed at 44100, looping streams, crossfaded music, and sound following entities; mixed on no device, by numbers |
| 16 | Loading textures as Blitz did | [textures_by_their_settings](textures_by_their_settings) | Color, alpha, mipmaps, and clamping by each file's sidecar; a strip's frames; PNG, JPEG, and BMP by their bytes, whatever their names; backslashed paths in any case; a sound whose name says nothing; what the cooker makes of each; a DDS refused plainly |
| 17 | Pixel access | [pixels_of_images_and_textures](pixels_of_images_and_textures) | An image's pixels read and written one at a time on the CPU, and a texture's, a target's and a plain one's, locked and written with alpha, and whether a million are quick |
| 18 | Graphics modes | [display_modes](display_modes) | The display's modes; windowed, fullscreen, and borderless at the desktop's size; a screen of its own size scaled in. A real display's, only under `--real-display` with FBENGINE_REAL_DISPLAY=1, as CI sets |
| 19 | INI reading and writing | [ini_settings](ini_settings) | Settings read, each to its kind, defaults for what's missing, changed, written, and read again, through std's Ini; a file that isn't one, by its line |
| 20 | Binary files | [binary_saves_and_maps](binary_saves_and_maps) | Saves and maps as ints, floats, bools, and length-prefixed strings, through std, in folders it makes, lists, and cleans up |
| 21 | Pathfinding over the waypoint graph | [pathfinding_over_a_graph](pathfinding_over_a_graph) | std's A* over a game's waypoints: the cheapest route, a door closing that sends it the long way, no way at all, and a graph that grows |
| 22 | Particles | [particles_on_sprites](particles_on_sprites) | A game's own particles over the engine's Sprites: seeded emitters of a template, with sub-templates bursting from each, rising, slowing, fading, turning, and growing |
| 23 | Procedural map generation | [procedural_map](procedural_map) | A map from a seed over seeded_random's generator, one stream as the requirement says, matched row for row by a Python tool apart from it; what the original's has beyond it is listed |
| 24 | Video | [two_videos_at_startup](two_videos_at_startup) | Two MPEG-1 videos, one with its sound in the file and one beside a sound of its own, each into a texture, fitted, and skipped by a key; what was shown at what was heard |
| 25 | Bump mapping | [bump](bump) | A BUMP layer shifting the next layer's UVs by its red and green and scaling its light by its blue, pixel by pixel |
| 26 | Texture detail, anti-aliasing, wireframe | [render_settings](render_settings) | A LOD bias, samples on a target and the screen, and wireframe, each by the pixels it changes. A Mac's samplers have no bias |
| 27 | Gamma | [gamma](gamma) | A frame's copy added over it, a gray multiplied over it, and a real curve by a table, in linear light and in stored values as DirectX 7 did |
| 28 | Text input | [text_input](text_input) | A console's line and a save's name, with text input on while one has the focus, and the raw keys and text a game reads itself |
| 29 | A debug UI | [debug_ui_and_console](debug_ui_and_console) | Rows asked for each frame, watched values, sliders and switches, and commands typed into a console over the game |

Item 30 (the updater, cpuid, DirectPlay, the launcher, and the rest) is dropped, so it has no sample.

Past section 6:

| Folder | What it shows |
|---|---|
| [stack_and_joints](stack_and_joints) | Physics as E10 and E13 give it, beyond what SCP:CB needed: a pyramid that stands, a block on a chain let go into it, a door a motor swings, a lift with a crate on it, and a fan, each piece a body driving an entity, run twice to show it goes the same |
