# Ideas for after 1.0

Things docs/plan.md doesn't plan, none of them committed to. Each has what it gives and a rough
size: S is days, M is weeks, L is months. Within a section, the most useful comes first.

## The ones I'd do first

1. **Compute pipelines and indirect draws** (M). Most of the rendering ideas below need them: Hi-Z
   culling, GPU particles that collide, GPU-driven scenes, light baking, and upscaling.
2. **A profiler** (M). CPU and GPU time per pass and per system, memory by kind, and draw counts, in
   a game's debug UI and the editor. Without it, performance work is guessing.
3. **Prefabs** (M). A saved piece of a scene, placed many times, with changes per copy. Scenes get
   hard to edit without them.
4. **Navigation meshes** (M). Built from a level's geometry, with paths and steering. The engine has
   only std's AStar over a graph that a game makes itself.
5. **Animation blending and state machines** (M). Any character beyond a few clips needs them.
6. **Testing on a real GPU** (S to M). CI's tests run mostly on software drivers, so a self-hosted
   runner with a real GPU, and frame times tracked over commits, would catch what those can't.

## Rendering

- **Dynamic global illumination** (L). Probe grids updated at run time, like DDGI, so light bounces
  without baking. The biggest step toward a AAA look. Needs compute.
- **A lightmap baker** (M). Games can use lightmaps but can't make them with the engine. Belongs in
  the editor, with light probes for what moves through a baked room.
- **Reflection probes** (M). Placed environment maps, blended by position, so a room reflects itself
  rather than the sky.
- **Screen-space reflections** (S to M). Wet floors and polished surfaces, as a game's own pass could
  do today, but shipped.
- **Volumetric fog and lights** (M). Light shafts and fog that shadows cut through.
- **Depth of field and motion blur** (S each). Standard post effects that aren't in E6.
- **Upscaling** (M). FSR, which is open, for high resolutions on weaker GPUs.
- **HDR output** (S to M). HDR10 to HDR displays; E6's HDR ends at an SDR image.
- **Terrain layers** (M). Several materials blended by a mask. Slack Water's island shows the need:
  one tile, visibly repeating.
- **A water material** (M). Waves, depth color, shoreline, and refraction.
- **Foliage** (M). Wind in the vertex shader, impostors for far trees, and placing thousands of
  plants on instancing.
- **LOD crossfade** (S). E6's LODs pop.
- **Subsurface scattering, clearcoat, and other glTF material extensions** (S each). Skin, car
  paint, cloth, and glass.
- **Dense geometry** (L). Meshlets and GPU culling, the road toward something like Nanite. Only
  worth it for very large scenes.
- **Ray tracing** (L). Ray queries for shadows and reflections where the GPU has them. Raises the
  bar above Vulkan 1.2's 2016 GPUs, so it must stay optional.
- **A material graph** (L). Shaders by nodes in the editor, on top of E6's custom shaders.

## World and content

- **World streaming** (L). Loading and unloading parts of a world and its textures on the job
  system, for maps too big for memory.
- **Texture streaming** (M). Only the mip levels a view needs in memory.
- **Level instancing** (S, with prefabs). The same room many times, as SCP:CB does.
- **Portals and rooms** (M). Occlusion by room graph, cheap and exact for indoor games.
- **A behavior tree or state machine package** (M). A small, shared way to write NPCs.
- **Cinematics** (M). A timeline of cameras, animation, and sound, with an editor.

## Animation and physics

- **Inverse kinematics** (M). Feet on stairs, looking at things, hands on objects.
- **Root motion** (S). Movement that comes from the clip.
- **Retargeting** (M). One clip on differently built skeletons.
- **Animation events** (S). A footstep sound at a clip's frame.
- **Ragdolls from a skeleton** (M). Going from animation to physics and back.
- **Cloth and soft bodies** (M). Jolt has soft bodies to build on.
- **Vehicles** (M). Jolt has them too.
- **Destruction** (L). Fractured meshes and debris.

## Networking

`std.net.game` gives hosts, peers, and channels. A game then writes the rest itself.

- **Replication** (L). An entity's state sent to clients by what's relevant to each, with
  interpolation. A package on top of `std.net.game`.
- **Client prediction and reconciliation** (M). So a player's own movement feels immediate.
- **Rollback** (M). For games of few players with deterministic steps; StateHash is a start.
- **Lag compensation** (M). Hits checked where the target was when the shooter fired.
- **NAT traversal and relays** (M). So friends can play without opening ports.
- **Voice chat** (M). Capture, Opus, and positional playback.
- **Lobbies and matchmaking** (M). With Steam's as one backend.

## Audio

- **Sound occlusion** (S to M). Walls muffle what's behind them, by picks.
- **Reverb by room** (S). Zones that set E9's reverb.
- **HRTF** (S to M). Positional sound for headphones.
- **A sound bank with variations** (S). Random pitch and choice, so steps don't repeat.
- **Subtitles** (S). Timed text tied to sounds, for accessibility.

## Editor and tools

- **Terrain sculpting and painting** (M).
- **A particle editor and an animation viewer** (M each).
- **Reloading game code in the editor** (L). Needs FutureBlitz's `Plugin.Load`, Deferred work there.
- **A frame debugger** (M). Each pass's output and each draw, in the editor.
- **Packaging** (M). One command for a game's installer per OS, signing included.
- **A crash reporter** (M). Stack traces and logs from players, with their yes.
- **Templates** (S). `fb new` with a first-person or 2D starting point.

## Platforms and players

- **Steam** (M). Achievements, cloud saves, friends, Workshop, and Deck's input.
- **Mods** (L). Loading assets and code from a folder; needs `Plugin.Load` for code.
- **Accessibility** (M). Remappable everything, text sizes, color-blind filters, and a screen reader
  for menus.
- **Gamepad button prompts** (S). The right glyph for the pad in use.
- **Consoles and phones** (L). Each is its own project; phones would fit Vulkan best.

## For the SCP:CB remake, outside the engine

The remake is its own project, and these belong there, or in tools tied to neither it nor the
engine.

- **A converter for .b3d, .x, and .rmesh** to glTF and fbengine's formats.
- **A feel comparison harness.** Scripted walks, recorded in the original and replayed in the
  remake, comparing position over time, to tune collisions and mouse look by numbers.
- **One random stream.** Item 1's generator shared by map, events, and gameplay, so one seed gives
  the same map and the same dice as the original.
- **A server-authoritative design from the start**, as multiplayer can't be added onto the
  original's per-frame, single-player logic.
