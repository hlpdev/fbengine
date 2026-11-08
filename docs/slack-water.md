# Slack Water

fbengine's showcase game, for E14: short, original, played in about ten minutes in daylight. Nothing
in it derives from SCP:CB, in setting, mechanics, mood, or structure: no horror, no facility, no
chasing, no blinking. It lives in `examples/slack_water/`. This is its approved design; what's been
added since is at its end.

## The game

You keep a tide-mill on a small rock island, and a storm's coming in. A tide gives you one slack
water, about ten minutes on a clock you can see, when the sea's still and the causeway is dry. In it
you restart the mill: find three things the last storm scattered, a gear, a sluice paddle, and a
drive belt, carry them back, and fit them, with Pip, a small two-legged brass helper who follows you
and carries what you hand it. Then you open the sluice, the wheel turns, and the lamp on the mill's
roof lights as the water comes back. Run out of time and the causeway floods; you can still win from
the mill's side, without what's left across it, in a poorer ending: the lamp lit, and dimmer.

## Why it's this

Each part has a reason to use an engine feature, and a person can see it working, in one place, in
the time a demo takes.

| Feature | What in the game needs it |
|---|---|
| PBR (E6) | wet rock, brass, oiled wood, and iron, side by side, as the sun lowers |
| Shadows (E6) | one sun, low and moving with the clock, the mill's shadow crossing the yard; the wheel's turning across the wall |
| Post effects (E6) | tone mapping and bloom on the lamp, a slight grade as the storm comes, and depth of field when you look at what you're holding |
| Skinned characters (E7) | Pip: walk, idle, carry, a head that turns toward you by a bone under the game's control, and a blinking eyelid by a morph target |
| Physics (E10) | the three parts are bodies, as are crates and barrels you push out of the way; the belt's a chain of bodies; the wheel's a motor on a hinge, driven by the sluice |
| Sound (E9, E12) | streamed wind and sea, positional gulls and creaks, the wheel by its speed, footsteps by surface, and a low-pass when you're inside |
| UI (E11) | the title over a flight round the island; Options (master, music, ambience, and effects volumes, quality, windowed or fullscreen at the display's sizes, look sensitivity and Invert Y, and keys rebound), kept in an INI; the tide clock; a prompt for what you can do, by the keys or the pad used last; pause; and the end screen |
| Pak and hot reload (E8) | a release runs from its pak; development reloads a texture or model as it's saved |
| Input (E1) | keyboard and mouse or a gamepad, by actions |
| Saves (std, E14's saves sample) | one, at the mill's door, so a player can stop |

## The world

One island, 200 m across: a yard, the mill and its inside, a causeway, and a second, smaller rock; an
open horizon with a sea plane past the edge. The sea's height rises by the clock, and it's a mesh and
a material, not a simulation. It doesn't have towns, other people, enemies, combat, inventory beyond
what your hands and Pip carry, or dialogue beyond a few signs.

## Assets

- Everything's made for this game, by code or in the repository, under the repository's own license,
  so `git log` is its provenance. No download, and no store assets.
- In code: the terrain's height and paint (fbengine's terrain, from noise), the sea, rock scatter,
  crates and barrels, the wheel, and procedural textures (noise made into albedo, roughness, and
  normal maps), written as PNGs by a tool in the game's folder, then cooked.
- In glTF files we make: Pip and its clips, from a small script of ours in the game's `tools/`, which
  builds its mesh, skin, and keyframes and writes a .gltf, so its source is code too.
- Sound: synthesized by a tool in the game's folder, wind and sea from filtered noise, creaks and
  gulls from small synth patches, written as WAV, and cooked as they are. Music too, decided for the
  showcase: a calm theme for slack water, a slow pad in D dorian with a few plucked notes, and a storm's,
  a pulsing low drone with a rubbing minor second and a drum at each bar. Each is a 16-second loop
  that meets its start, crossfaded by the weather and tide, under the cues, and quieter inside.
- One built-in font, as the engine's Font.BuiltIn is licensed already.
- If that looks too plain once E6 draws, the fallback is CC0 only, each listed in a NOTICES file. That
  question is open.

## Where it lives, and when it's done

A package on fbengine, fbengine_audio, fbengine_physics, fbengine_ui, and fbengine_tools' build step,
about 3000 lines of FB, with no feature of its own in the engine. Anything the game needs that's
missing goes back to its lane. It's done when, on Windows, Linux, and macOS, from a pak: its endings
can be reached by a walk in ctest; it holds 60 fps at 1080p on a mid-range GPU, by a number set then;
there are no validation errors; and a person has played it through once on each.

If it's long, cut in this order: the second rock and its ending; the belt as a chain (a plain part
instead); saves; depth of field.

## Added since

- **A third ending, DARK.** The page has two endings, as it has the sluice opened either way. Something
  has to happen when the tide comes in with the sluice shut: the wheel stands still, and the lamp stays
  dark.
- **The clock's two times.** It says the time until the flood, which is 2:10 at first, and the water
  rising in its last minute; then the time until the tide, 1:50 from the flood. The lines it says
  aren't stamped on the screen.
- **Its pace.** About four minutes, not ten, with a road of about 10 s to cross, 42 m at 4 m/s, between
  the rocks. Walked straight, by someone who knows the way, the last part's fitted at about 1:25, the
  belt off the road about 55 s before the flood; a first time may not make it. The text flow's walks
  and the island in 3D go by one map, examples/slack_water/map.fb, so a walk's length, and where the
  road starts and ends along it, are the same in both, though only 3D has its width.
- **The flood.** Whoever's on the road at 2:10 goes to the nearer shore. Everything lying on it or
  past it is the sea's, even where you're standing, and Pip, past the water, stays there with whatever
  Pip holds.
- **The wait.** Once nothing can change how it ends, the clock runs thirty times as fast, saying so, so
  finishing early isn't minutes of nothing.
- **The end.** Three seconds after it ends, a screen says how it ended and how many parts were fitted,
  and offers playing again or leaving, by the mouse, the keyboard, or a gamepad.
- **The look.** PBR throughout, rock, wood, brass, and iron with their albedo, normal, and roughness maps; a sky of its
  own, baked for the light around, which is what brass reflects; a sun with shadows, lowering by the
  clock; HDR, ACES, and bloom on the lamp; a grade as the storm comes, by the clock. The ground: a
  headland of crags, and a far rock's, by ridged noise, under a cover baked from height and slope (sand,
  wet shore, a tide line, rock where it's steep, turf, a path), and grit at two scales. The mill, of shapes built in code (beveled blocks, turned barrels, a gable):
  a plinth, corner stones, a framed door, a roof of boards to a ridge, a spoked wheel, the lamp in a
  cage, barrels and crates, a millstone, and a bench. Pip, rounded, of smooth parts on its joints, in
  brass, with white eyes and dark pupils that blink, and an antenna. Ambient
  occlusion and antialiasing over frames; `--low` has FXAA in their place. Depth of field, the first the
  design said to cut if it's long, isn't in.
- **The budget for 60 fps at 1080p**, 16.6 ms a frame on a mid-range GPU, as it's spent: the scene with
  its shadows 10 ms, ambient occlusion 2, antialiasing over frames 1, bloom and the tonemap 1, the canvas
  0.5, and 2 kept back; `--low`'s FXAA is 0.5 in place of the 3 that occlusion and TAA take. It's a
  budget: no real GPU's measured it yet. What each landmark costs of a frame of the whole island, on
  lavapipe, takes hundreds of frames to tell, so `--frames` says it only with SLACK_WATER_COSTS=1:
  the lighthouse 0.4%, the jetty 1.2%, the boat 1.5%, and what's along the shores 2.8%, of 522 draws
  and 148k triangles, a frame's time wandering 0.6%.
- **The tests' time.** The release is cooked once, by `examples.slack_water.cook`, which each test of
  it needs, and which fails past 120 s, saying what each file took; a fresh cook was 3 s on the
  machine it's made on. `--frames` says what each part of it took; it was 21 s there.
- **The number for 60 fps at 1080p.** On 2026-11-04, Slack Water lost the AMD integrated GPU of the
  machine it's made on within its first frames, each run, and the machine froze and reset. So `--fps`
  refuses a real GPU unless SLACK_WATER_REAL_GPU=1 and that GPU's the only device the loader shows,
  which on that machine means the RTX 4080, by its driver:

      VK_LOADER_DRIVERS_SELECT='*nv-vk64*' SLACK_WATER_REAL_GPU=1 SLACK_WATER_CHECKS=1 SDL_VIDEO_DRIVER=offscreen fb run --release examples/slack_water -- --fps 0 300

  A mid-range GPU means an RTX 3060-class one. As only the 4080 can be measured, its number is a third
  of a 60 fps frame: a median GPU frame of 5.5 ms and a 99th percentile of 7 ms, at 1080p with vsync
  off, by the command above (decided 2026-11-05). Why the AMD was lost is still open. On lavapipe,
  under GPU-assisted validation, Slack Water has no out-of-bounds address or index, and sync
  validation's clean, which doesn't say why.
