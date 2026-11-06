# The proofs

A sample for each engine requirement in [section 6 of the requirements](../../docs/research/scpcb-requirements.md#6-engine-requirements),
as E14 asks. Each runs on fbengine's own features, and what a game owns, such as its random generator
or its room format, is the sample's own code. Each prints what it did, by numbers worked out apart
from the code, and `tests/CMakeLists.txt` checks those lines.

| Item | Pri | What it asks for | Folder |
|---|---|---|---|
| 1 | P0 | A seeded random generator | [seeded_random](seeded_random/main.fb) |
| 2 | P0 | Swept collisions | [swept_collisions](swept_collisions/main.fb) |
| 3 | P0 | Picks and visibility | [picks](picks/main.fb) |
| 4 | P0 | In view, by bounds and planes | [in_view_by_bounds](in_view_by_bounds/main.fb) |
| 5 | P0 | The scene as a tree | [scene_trees](scene_trees/main.fb) |
| 6 | P0 | Meshes built in code | [meshes_in_code](meshes_in_code/main.fb) |
| 7 | P0 | The fixed-function look | [fixed_function_look](fixed_function_look/main.fb) |
| 8 | P0 | A room format, by a game's own reader | [room_files](room_files/main.fb) |
| 9 | P0 | Window, frame timing, and input | [frame_timing_and_input](frame_timing_and_input/main.fb) |
| 10 | P0 | Several cameras in a frame | [cameras_and_viewports](cameras_and_viewports/main.fb) |
| 11 | P0 | 2D over 3D | [sprites_over_a_scene](sprites_over_a_scene/main.fb) |
| 12 | P0 | Render to texture and screen copies | [camera_into_a_texture](camera_into_a_texture/main.fb) |
| 13 | P1 | Skinned animation, bones under game control | [skinned_animation_with_bones_under_game_control](skinned_animation_with_bones_under_game_control/main.fb) |
| 14 | P1 | What a game gets of a converter's glTF | [models_as_a_converter_writes_them](models_as_a_converter_writes_them/main.fb) |
| 15 | P1 | Audio: channels, streams, and position | [streamed_and_positional_sound](streamed_and_positional_sound/main.fb) |
| 16 | P1 | Textures by their settings and files, and sound files by their bytes | [textures_by_their_settings](textures_by_their_settings/main.fb), and [streamed_and_positional_sound](streamed_and_positional_sound/main.fb) for Ogg Vorbis and WAV .cwm files by backslashed paths |
| 17 | P1 | Pixel access | [pixels_of_images_and_textures](pixels_of_images_and_textures/main.fb) |
| 18 | P1 | Graphics modes | [display_modes](display_modes/main.fb) |
| 19 | P1 | INI reading and writing | [ini_settings](ini_settings/main.fb) |
| 20 | P1 | Binary files, folders | [binary_saves_and_maps](binary_saves_and_maps/main.fb) |
| 21 | P1 | Pathfinding over a graph | [pathfinding_over_a_graph](pathfinding_over_a_graph/main.fb) |
| 22 | P1 | Particles as a game's code | [particles_on_sprites](particles_on_sprites/main.fb) |
| 23 | P1 | A map from a seed | [procedural_map](procedural_map/main.fb) |
| 24 | P2 | Two videos at startup | [two_videos_at_startup](two_videos_at_startup/main.fb) |
| 25 | P2 | Environment bump | [bump](bump/main.fb) |
| 26 | P2 | LOD bias, anti-aliasing, wireframe | [render_settings](render_settings/main.fb) |
| 27 | P2 | Gamma, as a game does it | [gamma](gamma/main.fb) |
| 28 | P2 | Text input | [text_input](text_input/main.fb) |
| 29 | P2 | A debug UI and console | [debug_ui_and_console](debug_ui_and_console/main.fb) |
| 30 | drop | Nothing: the section drops it | none |

Two go past section 6:

| What it shows | Folder |
|---|---|
| Physics: a stack and what joints do | [stack_and_joints](stack_and_joints/main.fb) |
| E6 step 7: materials with shaders of their own | [materials_with_their_own_shaders](materials_with_their_own_shaders/main.fb) |

## Running one

```bash
fb run examples/proof/bump
```

Add `-- --watch` for a window a person watches, and `-- --watch --frames 40` for that many frames with
no window, which is how the tests walk it. There, on its last frame, a sample reads what it drew and
prints it, so a test checks the frame as well as the numbers. `tools/show.py` opens them too.

## Their files

streamed_and_positional_sound's tone.ogg is the repository's own, the same file as examples/sound's
and fbengine_audio's tests/data/tone.ogg: a quarter second of 440 Hz in stereo, made by ffmpeg with
the command its main.fb's header gives.

## What takes a person

A flag can't show how long a flip waits, so timing it takes a window and a display:

```bash
fb run examples/proof/frame_timing_and_input -- --watch
```

Read "a second" on its first line. With vsync on, it's the display's refresh; press V to turn vsync off,
and it goes well past it.
