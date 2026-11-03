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
| UI (E11) | the title, settings (volume, fullscreen, keys rebound), the tide clock, a prompt for what you can do, pause, and the end screen |
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
  gulls from small synth patches, written as WAV, and cooked as they are. Music is left out unless
  it's wanted.
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
- **The clock's two times.** It says the time until the flood, 2:10, then until the tide, 4:00, each
  named, and warns at 1:10, when the water starts to rise.
- **Its pace.** About four minutes, not ten, with a road of about 10 s to cross, 42 m at 4 m/s, between
  the rocks. Walked straight, by someone who knows the way, the last part's fitted at about 1:25, the
  belt off the road about 55 s before the flood; a first time may not make it. The text flow's walks
  and the island in 3D go by one map, examples/slack_water/map.fb, so they can't drift.
- **The flood.** Whoever's on the road at 2:10 goes to the nearer shore. Everything lying on it or
  past it is the sea's, even where you're standing, and Pip, past the water, stays there with whatever
  Pip holds.
- **The wait.** Once nothing can change how it ends, the clock runs thirty times as fast, saying so, so
  finishing early isn't minutes of nothing.
