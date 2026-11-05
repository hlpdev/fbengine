# Scene files

What `Scene.Save` writes and `Scene.Load` reads, in fbengine's `packages/fbengine/scene_file.fb`: TOML, one
table to a thing, so a change to an entity is a change to its own lines.

```toml
format = 1

[[material]]
id = "room/crate.material"
color = [1.0, 0.5, 0.25, 1.0]
shading = "PBR"
blend = "AUTO"

[[material.layers]]
layer = 0
texture = "res://wall.png"
blend = "MULTIPLY"
uv_set = 0
uv = [1.0, 1.0, 0.0, 0.0, 0.0]
bump = [0.0, 0.0, 0.0, 0.0, 1.0, 0.0]

[[geometry]]
id = "room/crate.geometry"
source = "builtin:sphere:5"

[[entity]]
path = "room"
kind = "PIVOT"
name = "room"
position = [1.0, 2.0, 3.0]
rotation = [0.0, 0.70710677, 0.0, 0.70710677]
scale = [1.0, 1.0, 1.0]

[entity.components.body]
mass = 2.5

[[entity]]
path = "room/crate"
kind = "MESH"
geometry = "res://door.glb#0"
material = "room/crate.material"
```

- **format**: 1. Any other is refused.
- **[[entity]]**: `path` is its id: its parent's path, a `/`, and its name, with `/` in the name as `_`, none
  as `entity`, and the first free of `#2` and on after it for a name a sibling has. A parent comes before what's
  under it. So a same-named sibling put before another takes its id, shifting the others' paths.
  `kind` is PIVOT, MESH, LIGHT, CAMERA, or SPRITE. What's left out is as a new one of that kind has it, and
  all but the transform and what's a Light's, Camera's, or Sprite's own are written only when they aren't that.
- **[entity.components.\<name\>]**: what `Entity.SetComponent` set, read and written as it is. `Scene.Load` also
  makes each with `Components.Attach`, by the kind called `<name>`, and it's an error if it can't, so the `body`
  above loads only in a game that's added a kind called that; `Scene.Open`, as the editor reads a scene, takes any.
  A component with what TOML can't write, a list holding Null or a whole number past an Int64, refuses the save,
  naming it. A Field set to Null is left out, so it's read back as what a new one has.
- **geometry** of a mesh: `builtin:cube`, `builtin:sphere:<segments>`, `builtin:plane:<segments>`,
  `builtin:cylinder:<segments>:capped` or `:open`, `builtin:cone:...`, a mesh of its own each; a model's mesh,
  `res://door.glb#0`, its number after the last `#`; or `geometry:<id>` for a `[[geometry]]`, shared by what
  names it. A table has a `source`, for what's built in and shared, or `surfaces` of `vertices`, each 14 numbers
  (position, normal, two UVs, color), and flat `triangles`, with an optional `material` and `cull_box`.
- **[[material]]**: `id`s are kept from one save to the next; a new one's is its first user's path and what it is
  to that (`.material`, `.surface0`, `.geometry`), so changing a material changes its own lines. Its PBR numbers,
  `metallic`, `roughness`, `emissive`, `normal_scale`, and `occlusion_strength`, are written when they aren't what a
  new one has, as is each of its `[[material.maps]]`: `map` is METAL_ROUGH, NORMAL, OCCLUSION, or EMISSIVE, with a
  `texture`, `uv_set`, and `uv`. A map but EMISSIVE isn't a color, so its file's import settings say it isn't sRGB,
  and `Scene.Load` refuses one that is. A layer's or a map's `texture` is an image's file, or one of a model's:
  `res://door.glb#2` by its image's number, with `:1` and on for another Texture of that image, read as other than
  its first is. A texture of neither can't be saved, and `Scene.Save` says which entity has it.
- A **light**'s shadows are `shadows`, `shadow_size`, `shadow_bias`, `cascades`, and `shadow_distance`; a **camera**'s
  picture `hdr`, `exposure`, `tonemap`, `gamma`, `bloom`, `grade`, `occlusion`, `ambient_occlusion`,
  `ambient_occlusion_radius`, `fxaa`, and `temporal_aa`; and any entity's `occluder` and `casts_shadows`. Each is
  written when it isn't what a new one has.
- Numbers are Float32's shortest text, so read and written again they're the same to the byte. They're finite.

What a setter would panic at is an IOError of INVALID_DATA: a group outside 0 to 1023, a range of 0, a wrong type.
A key the file has that a table can't have is one too, but an entity's: that loads, and then saving it refuses,
naming it, as it'd be lost. `Scene.Open`, the editor's, lets what a file refers to be missing: it stands in, keeps
its reference for saving, and it's in `Problems`.

Not saved: a Skin (a model's skinned mesh loads in its bind pose), a game's `SetData`, a camera's viewport and
target, a World's ambient and collision responses, planes, terrains, and mirrors (saving those refuses, naming
them), and a changed geometry's morph targets (also refused). Vertices are a line to each array, until std's Toml
writes long ones an element a line, so two edits to them don't merge.
