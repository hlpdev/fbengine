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
- **[entity.components.\<name\>]**: whatever's set by `Entity.SetComponent`, read and written as it is.
  `Scene.Load` also makes each with `Components.Attach`, by the kind called `<name>`, and it's an error if it
  can't; `Scene.Open`, as the editor reads a scene, doesn't.
- **geometry** of a mesh: `builtin:cube`, `builtin:sphere:<segments>`, `builtin:plane:<segments>`,
  `builtin:cylinder:<segments>:capped` or `:open`, `builtin:cone:...`, a mesh of its own each; a model's mesh,
  `res://door.glb#0`, its number after the last `#`; or `geometry:<id>` for a `[[geometry]]`, shared by what
  names it. A table has a `source`, for what's built in and shared, or `surfaces` of `vertices`, each 14 numbers
  (position, normal, two UVs, color), and flat `triangles`, with an optional `material` and `cull_box`.
- **[[material]]**: `id`s are kept from one save to the next; a new one's is its first user's path and what it is
  to that (`.material`, `.surface0`, `.geometry`), so changing a material changes its own lines.
- Numbers are Float32's shortest text, so read and written again they're the same to the byte. They're finite.

What a setter would panic at is an IOError of INVALID_DATA: a group outside 0 to 1023, a range of 0, a wrong type.
A key the file has that a table can't have is one too, but an entity's: that loads, and then saving it refuses,
naming it, as it'd be lost. `Scene.Open`, the editor's, lets what a file refers to be missing: it stands in, keeps
its reference for saving, and it's in `Problems`.

Not saved: a Skin (a model's skinned mesh loads in its bind pose), a game's `SetData`, a camera's viewport and
target, a World's ambient and collision responses, planes, terrains, and mirrors (saving those refuses, naming
them), and a changed geometry's morph targets (also refused). Vertices are a line to each array, until std's Toml
writes long ones an element a line, so two edits to them don't merge.
