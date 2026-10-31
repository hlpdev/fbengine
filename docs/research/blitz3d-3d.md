# Blitz3D's 3D commands in FutureBlitz and fbengine

Group "3d": every command in `bbruntime/bbblitz3d.cpp` (234), read against the source in
`blitz3d/` (the engine), `gxruntime/gxscene.cpp`, `gxlight.cpp`, `gxcanvas.cpp`, and `ddutil.cpp`
(the DX7 layer), and the help in `_release/help/commands/3d_commands/`. Where the help and the
source disagree, the source wins and the note says so. File and function names below are
Blitz3D's, relative to its repository.

## Conventions every row uses

These are decided once here so the rows can say "converted":

- **Space.** fbengine uses std.math's space: right-handed, +Y up, -Z forward, angles in radians
  (std/math/vector.fb, quat.fb, matrix.fb). Blitz3D is left-handed, +Y up, +Z forward, degrees.
  The mirror between them is M = diag(1, 1, -1), so at each call the migrator converts:
  - a point, vector, or normal `x, y, z` becomes `Vec3(x, y, -z)`, and each one read back has its
    z negated;
  - Euler angles `pitch, yaw, roll` (degrees) become `-Deg(pitch), Deg(yaw), Deg(roll)`, and
    each read back is `-ToDegrees(e.Pitch())`, `ToDegrees(e.Yaw())`, `ToDegrees(e.Roll())`.
    Why: Blitz's rotation is `yawMatrix(y) * pitchMatrix(p) * rollMatrix(r)` (geom.h
    `rotationMatrix`, geom.cpp `rotationQuat`), and M times that times M is
    `Ry(y) * Rx(-p) * Rz(r)` in right-handed terms, which is exactly std's
    `Quat.FromEuler(-p, y, r)` (Y, X, Z order: roll, then pitch, then yaw). So fbengine's Euler
    order is std's, only pitch changes sign.
  - What the signs mean in Blitz (geom.h): +pitch tips the nose down (`pitchMatrix` takes +Z to
    (0, -sin, cos)), +yaw turns left, counterclockwise seen from above (+Z goes to (-sin, 0, cos)),
    +roll rolls left (the right side goes up). In fbengine the same motions are -pitch, +yaw,
    +roll, so "turn left" and "roll left" keep their signs.
  - a Blitz `x, y, z, width, height, depth` box becomes `AABB(Vec3(x, y, -(z + depth)),
    Vec3(x + width, y + height, -z))`;
  - triangles: Blitz's front faces are clockwise as seen (DX7 culls counterclockwise,
    gxscene.cpp `setTriCull`). Negating z keeps what's seen but flips the cross product, so the
    migrator swaps two corners: `AddTriangle s, a, b, c` becomes `s.AddTriangle(a, c, b)`, and
    `TriangleVertex`'s corner 1 and 2 swap. fbengine then uses glTF's and Vulkan's
    counterclockwise front faces everywhere. The mesh converter does the same to files.
  - matrices: an element of Blitz's world matrix at row r (the basis vector i, j, k, or the
    translation) and column c (its component) is std's `m[r, c]` (column r, component c, since
    std's Mat4 columns are the basis vectors), negated when exactly one of r and c is 2 for
    r < 3, and when c is 2 for r = 3.
- **Colors.** Blitz takes 0 to 255 as floats and doesn't clamp them (negative and over-255 light
  colors are a known trick). The migrator writes `Color.Srgb(r / 255, g / 255, b / 255)`. std's
  ToLinear and ToSrgb (std/math/color.fb) invert each other over all reals, so `Shading.VERTEX`
  recovers Blitz's exact numbers and does DX7's math on them as DX7 did, in display space (see
  design question 5). Alpha stays a Float from 0 to 1.
- **Indices.** `GetSurface`, `GetChild`, and every collision index are 1-based in Blitz and
  0-based in fbengine; the migrator subtracts 1. Vertices, triangles, texture layers, frames,
  cube faces, and animation sequences are 0-based in both.
- **Handles and failure.** Blitz's 0 handle is Null. Loads that give 0 on failure throw in
  fbengine (E8's asset error); the migrator turns a Blitz `If x = 0` test after a load into a
  catch. Freed handles are generational: Blitz crashes or corrupts memory, fbengine raises a
  clear error.
- **global.** Blitz's `global` flag is `Space.WORLD`, and its default is `Space.LOCAL`, whose
  meaning is per method, as Blitz's is: the parent's space for SetPosition, SetScale, Translate,
  and Position; the entity's own axes for Turn.
- **Last-result globals.** Blitz keeps the last pick, collision query, TForm, and projection in
  globals (`PickedX`, `TFormedX`, `ProjectedX`). fbengine returns Structs instead (`Pick`,
  `Collision`, a `Vec3`). The migrator keeps Blitz's globals, with their exact staleness, in its
  support module `fbengine.blitz` (E13), next to small converters (`Blitz.Vec`, `Blitz.Rgb`,
  `Blitz.Box`, `Blitz.EntityClass`).
- **Typing.** Blitz handles are Ints; fbengine's are Classes: `Entity`, its subclasses `Pivot`,
  `Camera`, `Light`, `Mirror`, and `Model` (Blitz's own name for what has a brush), whose
  subclasses are `Mesh`, `Sprite`, `GroundPlane`, `Terrain`, `Md2Model`, and `BspModel`. Blitz
  checks kinds at run time in debug builds ("Entity is not a model"); fbengine checks at compile
  time, so the migrator infers each variable's class (design question 13).
- Every Method and Function below that reaches the engine is `@MainThread`; none is a Property.

Proposed names that recur: `Space` (LOCAL, WORLD), `Axis` (RIGHT, UP, FORWARD),
`Material` (Blitz's brush, as engine.md's `mat.SetShading`), `Geometry` (a mesh's shared
surfaces, design question 3), `Effects` (EntityFX's flags as Bool Fields), `Blend` (AUTO, ALPHA,
MULTIPLY, ADD), `TextureBlend` (NONE, ALPHA, MULTIPLY, ADD, DOT3, MULTIPLY2), `TextureOptions`
(assets.md's), `Pick` and `Collision` (Structs of Entity, Point, Normal, Time, Surface,
Triangle).

## World and the 3D mode

| Blitz3D | What it does (one short line, from the help and source) | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `LoaderMatrix file_ext$,xx#,xy#,xz#,yx#,yy#,yz#,zx#,zy#,zz#` | Sets the axis matrix applied to every mesh loaded from files with that extension | the mesh converter's `axes: Mat3` option | fbengine_tools E13 | Defaults (bbblitz3d.cpp `blitz3d_open`): "x" identity, "3ds" Y and Z swapped, "b3d" identity (absent from the map, so a default Transform). The three triples are the matrix's i, j, k columns. A runtime no-op once files are converted: the migrator records each call and hands the matrix, as M·A·M, to the converter for that extension. |
| `HWMultiTex enable` | Turns hardware multitexturing on (default) or off; off draws each layer past the first as another blended pass | dropped | dropped | Every Vulkan GPU samples eight layers in one pass, and E5 does them in one shader. The migrator emits nothing. The multipass fallback (gxscene.cpp `render`) turned lighting off for the extra passes; no game depends on it. |
| `HWTexUnits()` | How many texture stages the card blends in one pass | `8` | dropped | The migrator emits the literal 8, E5's layer count. |
| `GfxDriverCaps3D()` | 110 when the card has cube maps, else 100 | `110` | dropped | Every target has cube maps; the migrator emits 110. |
| `WBuffer enable` | Uses a W-buffer instead of a Z-buffer (16-bit depth only) | dropped | dropped | Depth is 32-bit float; nothing to choose. |
| `Dither enable` | DX7 dithering, on by default | dropped | dropped | No 16-bit targets. |
| `AntiAlias enable` | D3D7's sort-independent edge antialiasing, off by default | `Renderer.SetMsaa(enable ? 4 : 1)` | fbengine E6 | DX7's edge AA rarely did anything on real cards; MSAA is E6's. |
| `WireFrame enable` | Draws every triangle as lines, for every camera, until turned off | `Renderer.SetWireframe(enable)` | fbengine E5 | Global state (gxscene.cpp `setWireframe`); needs Vulkan's fillModeNonSolid, which desktop GPUs and MoltenVK have. |
| `AmbientLight red#,green#,blue#` | Sets the global ambient light, 127,127,127 by default | `World.SetAmbientLight(Color.Srgb(r / 255, g / 255, b / 255))` | fbengine E4 (lit by E5) | Default is int(0.5 * 255) = 127 per channel (gxscene.cpp's GRAY). The ambient term is ambient × the material's color (D3D's material ambient is set to the diffuse color, gxscene.cpp `setRenderState`), or × the vertex color with FX 2. FX 1 swaps it for white. Not clamped: `gxScene::setAmbient` packs `int(v*255)<<16` etc., so values past 255 or below 0 bleed into the next channel; clamp to 0..255 and note the difference. BSP models use BSPAmbientLight instead. |
| `UpdateWorld [elapsed#=1]` | Animates every shown entity by `elapsed`, then moves each one with a collision type through Blitz's collisions | `UpdateWorld(elapsed)` | fbengine E4 (animation E7) | The order and its traps are in Behaviors (world.cpp `World::update`, `World::collide`). `elapsed` scales animation, MD2 frames, and transitions only, never movement. Velocity, for 3D sound's Doppler, is the move divided by `elapsed`, so 0 divides by zero. |
| `CaptureWorld` | Saves each shown entity's local position, rotation, scale, and alpha (and a sprite's rotation and scale) for RenderWorld's tween | `CaptureWorld()` | fbengine E4 | Only visible entities are captured (world.cpp `capture` uses enumVisible). Color isn't captured, though the help says so. |
| `RenderWorld [tween#=1]` | Renders every shown camera, mirrors first; a tween below 1 draws each captured entity between its captured and current state | `RenderWorld(tween)` | fbengine E4 (the look, E5) | Tweening lerps the local position and scale, slerps the local rotation, lerps alpha, and composes down the hierarchy (object.cpp `beginRender`, `getRenderTform`); never-captured entities render as they are. Also updates the 3D listener and every playing emitter (world.cpp `render`, object.cpp `updateSounds`). Beta builds' F11 stats and F12 screenshot keys aren't in release builds: dropped. |
| `ClearWorld [entities=1][,brushes=1][,textures=1]` | Frees every entity, brush, and/or texture | `World.Clear(entities, materials, textures)` | fbengine E4 | Frees top-level entities, their children with them (bbblitz3d.cpp `bbClearWorld`). With materials refcounted (design question 4), `materials` only drops the engine's own references. |
| `ActiveTextures()` | How many texture images are alive | `World.ActiveTextures()` | fbengine E4 | Counts the image cache's entries (cachedtexture.cpp `active_texs`), not handles: two loads of one file with one set of flags count once. |
| `TrisRendered()` | Triangles the last RenderWorld drew | `World.TrianglesRendered()` | fbengine E4 | Every camera and mirror pass counts; a sprite is 2. |
| `Stats3D(type)` | Undocumented debug counters | `World.Stats()`, a `SceneStats` Struct | fbengine E4 | Index 0: collision triangles tested since the last UpdateWorld began, picks included (meshcollider.cpp); 1 and 2: the most terrain vertices and triangles ever drawn (terrainrep.cpp); the rest 0. Low priority. |

## Textures

| Blitz3D | What it does (one short line, from the help and source) | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `CreateTexture(width,height[,flags=0][,frames=1])` | Makes a blank texture of `frames` frames | `Texture(width, height, options, frames)` | fbengine E4 (flags' effects, E5) | The size rounds up to a power of two in each direction (ddutil.cpp `adjustTexSize`, always), and TextureWidth reports that; a game drawing 100 pixels into CreateTexture(100,100) fills 100/128 of the UV space (design question 11). Texture filters are not applied, so flags 0 means no mipmaps (texture.cpp `Texture(w,h,flags,cnt)`). Mask implies color and alpha. 0 frames is an error. |
| `LoadTexture(file$[,flags=1])` | Loads an image as a texture | `Texture.Load(path, options)` | fbengine E4 (flags' effects, E5) | The flags are OR'd with every filter whose text is in the lowercased path, and the default filter, "" → 1+8, makes every load mipmapped (texture.cpp `filterFile`, bbblitz3d.cpp `blitz3d_open`). The image is stretched to the power-of-two size. Alpha rules (ddutil.cpp `buildMask`, `buildAlpha`): mask (4) sets alpha 0 where RGB is exactly 0,0,0 and 255 elsewhere; alpha (2) on an image with no alpha channel makes alpha (r+g+b)/3, and also whitens RGB when flag 1 isn't set; without flag 2 alpha is 255. Mipmaps are 2x2 box-filtered. Two loads of one path and flag set share pixels (cachedtexture.cpp `findRep`) but each has its own blend, coords, and transform (design question 12). Fails with 0 for a missing file. |
| `LoadAnimTexture(file$,flags,width,height,first,count)` | Loads `count` frames cut from an image of `width` × `height` cells | `Texture.LoadFrames(path, options, Vec2i(width, height), first, count)` | fbengine E4 | Cells go left to right, then down, starting at cell `first`; it fails when `first + count` passes the number of whole cells (cachedtexture.cpp `Rep`). Each frame is its own power-of-two image. With the cube flag the strip is six faces, as LoadTexture's. |
| `FreeTexture texture` | Frees a texture handle | `texture.Free()` | fbengine E4 | Blitz frees the handle only: every brush and entity using it keeps its own reference and goes on drawing it (texture.cpp's refcounted Rep). fbengine does the same: Free drops the program's reference, and the pixels go when the last material lets go. |
| `TextureBlend texture,blend` | How a texture layer combines with the layers below it | `texture.SetBlend(TextureBlend.MULTIPLY)` | fbengine E5 | Default 2, multiply (texture.cpp Rep). 0 drops the layer, and the layers above it move down a stage (gxscene.cpp `setRenderState` skips it). 1: lerp from the stage below to the texture by the texture's alpha (D3DTOP_BLENDTEXTUREALPHA). 2: modulate. 3: add. 4: DOT3 of the two as signed vectors. 5: modulate times 2. Each stage's alpha is the texture's alpha times the stage below when the texture has the alpha flag, else the stage below's (`setTexState`). Stage 0's "below" is the lit vertex color. |
| `TextureCoords texture,coords` | Which UV set a layer reads, 0 or 1 | `texture.SetUvSet(set)` | fbengine E5 | Sphere and cube maps ignore it. A property of the Texture, so shared by every material using it. |
| `ScaleTexture texture,u_scale#,v_scale#` | Scales a texture's UVs | `texture.SetUvScale(Vec2(uScale, vScale))` | fbengine E5 | Stored as 1/u, 1/v, so 2 makes the texture look twice as big (bbblitz3d.cpp `bbScaleTexture`). The transform, with s = 1/scale, r = -angle, and t = -offset (texture.cpp `getMatrix`, D3D's row vectors): u' = s.u·(cos r·u + sin r·v) + t.u, v' = s.v·(-sin r·u + cos r·v) + t.v. It turns about UV 0,0, the image's top-left, not its center. Shared by every user of the texture. Convert nothing: UV 0,0 is the top-left in DX7, Vulkan, and glTF alike. |
| `RotateTexture texture,angle#` | Turns a texture's UVs, in degrees | `texture.SetUvRotation(Deg(angle))` | fbengine E5 | Stored negated (bbblitz3d.cpp `bbRotateTexture`); see ScaleTexture for the formula. |
| `PositionTexture texture,u_offset#,v_offset#` | Offsets a texture's UVs | `texture.SetUvOffset(Vec2(u, v))` | fbengine E5 | Stored negated, and added after the scale and turn, so in final UV units. |
| `TextureWidth(texture)` | The texture's width in texels | `texture.Width()` | fbengine E4 | Frame 0's size after rounding up to a power of two. |
| `TextureHeight(texture)` | The texture's height in texels | `texture.Height()` | fbengine E4 | As TextureWidth. |
| `TextureName$(texture)` | The file a texture came from | `texture.Name()` | fbengine E4 | Blitz gives the lowercased full path it resolved (cachedtexture.cpp), "" for a created texture. The migrator lowercases if the game compares. |
| `SetCubeFace texture,face` | Picks the cube face that TextureBuffer draws to | `texture.SetCubeFace(CubeFace.FRONT)` | fbengine E5 | 0 left (-X), 1 front (+Z), 2 right (+X), 3 back (-Z), 4 up, 5 down (gxcanvas.cpp's constructor). Named by direction, so FRONT stays "forward" after the z flip and the migrator maps the numbers straight. A loaded cube map leaves face 1 selected. |
| `SetCubeMode texture,mode` | How a cube map's lookup vector is made | `texture.SetCubeMode(CubeMode.REFLECTION, CubeSpace.WORLD)` | fbengine E5 | 1 reflection (default), 2 normal, 3 position, each in camera space; adding 4 keeps it in camera space, else the inverse view rotation takes it to world space (gxscene.cpp `setTexState`). |
| `TextureBuffer(texture[,frame=0])` | A 2D buffer to draw into the texture with | `texture.Target(frame)`, E3's draw target | fbengine E4 | 0 for a frame out of range (v1.04's fix). Drawing goes through E3's CPU staging path; the cube face is SetCubeFace's. |
| `ClearTextureFilters` | Removes every texture filter, the default one too | `TextureFilter.Clear()` | fbengine E5 | Loads after it are no longer mipmapped unless asked. |
| `TextureFilter match_text$[,texture_flags=0]` | Adds flags to every texture loaded whose name contains the text | `TextureFilter.Add(match, options)` | fbengine E5 | Matched against the lowercased path, case-insensitively, OR'd in order (texture.cpp `filterFile`). Applies to LoadTexture, LoadAnimTexture, LoadBrush, LoadSprite, and textures meshes load; not to CreateTexture. Graphics3D restores the default "" → 1+8. |

## Brushes (materials)

Blitz's brush is fbengine's `Material`: color, alpha, shininess, eight texture layers (each a
texture and a frame), a blend, and FX. Painting copies a brush (brush.cpp copy-on-write), so
changing a brush after PaintEntity, PaintSurface, or PaintMesh doesn't change what was painted;
the migrator emits `.Clone()` at each paint while Material is a shared Class (design question 4).

| Blitz3D | What it does (one short line, from the help and source) | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `CreateBrush([red#=255][,green#=255][,blue#=255])` | Makes a brush of a color | `Material(Color.Srgb(r / 255, g / 255, b / 255))` | fbengine E4 | Defaults (brush.cpp `Rep`): alpha 1, shininess 0, blend 0 (auto), FX 0, no textures. |
| `LoadBrush(file$[,texture_flags=1][,u_scale#=1][,v_scale#=1])` | Makes a white brush with one loaded texture, scaled | `Material.Load(path, options, Vec2(uScale, vScale))` | fbengine E4 | The scale is ScaleTexture's, on that texture. Blitz deletes the file name twice (bbblitz3d.cpp `bbLoadBrush`), a bug not to copy. 0 when the texture doesn't load. |
| `FreeBrush brush` | Frees a brush | `material.Free()`, or dropped | fbengine E4 | Painted surfaces and entities keep their copies. Whether Material needs Free is design question 4. |
| `BrushColor brush,red#,green#,blue#` | Sets a brush's color | `material.SetColor(color)` | fbengine E4 | Unclamped, as all Blitz colors. |
| `BrushAlpha brush,alpha#` | Sets a brush's alpha | `material.SetAlpha(alpha)` | fbengine E4 | Below 1 makes the auto blend alpha (brush.cpp `getBlend`); 0 isn't drawn (model.cpp `beginRender`). |
| `BrushShininess brush,shininess#` | Sets a brush's specular shininess, 0 to 1 | `material.SetShininess(shininess)` | fbengine E4 (lit by E5) | Specular color is the shininess clamped to 0..1 as gray, and the power is shininess × 128 (gxscene.cpp `setRenderState`). |
| `BrushTexture brush,texture[,frame=0][,index=0]` | Puts a texture's frame on one of a brush's eight layers | `material.SetTexture(texture, frame, layer)` | fbengine E4 | Layers 0 to 7 (gxScene::MAX_TEXTURES). Texture animation is calling this with another frame. |
| `GetBrushTexture(brush[,index=0])` | A brush's texture at a layer | `material.Texture(layer)`, a `Texture?` | fbengine E4 | Blitz makes a new handle each call, sharing the texture, which leaks unless freed, and gives an empty texture for an empty layer (TextureWidth on it crashes). fbengine gives the same Texture, or Null. |
| `BrushBlend brush,blend` | Sets how a brush blends with the screen | `material.SetBlend(Blend.ALPHA)` | fbengine E4 (drawn by E5) | 0 auto (the default), 1 alpha, 2 multiply, 3 add; see EntityBlend. |
| `BrushFX brush,fx` | Sets a brush's FX flags | `material.SetEffects(Effects(fullBright: True))` | fbengine E4 (drawn by E5) | The flags are EntityFX's. |

## Meshes

A Blitz mesh entity owns its surfaces through a shared, refcounted rep (meshmodel.cpp
`MeshModel::Rep`): CopyEntity shares it, so ScaleMesh on a copy changes the original, as the
help says. fbengine makes that visible: a `Mesh` entity draws a `Geometry`, and `mesh.Copy()`
shares it (design question 3).

| Blitz3D | What it does (one short line, from the help and source) | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `LoadMesh(file$[,parent])` | Loads a mesh file as one mesh, its hierarchy collapsed | `Mesh.Load(path, parent)` | fbengine E4 | glTF through fastgltf; .x, .3ds, and .b3d once converted (fbengine_tools E13). Collapsing bakes each node's world transform into its vertices and merges surfaces with equal brushes (bbblitz3d.cpp `collapseMesh`, meshmodel.cpp `add`); the result has no animation. Blitz gives 0 for any other extension. Textures are looked for in the mesh's folder first, by file name (cachedtexture.cpp). |
| `LoadAnimMesh(file$[,parent])` | Loads a mesh file keeping its hierarchy, bones, and animation | `Entity.Load(path, parent)` | fbengine E4 (animation E7) | Gives the root, which can be a Pivot, so it's an `Entity`. The file's animation is sequence 0, posed at frame 0 and not animating (`animate(1,0,0,0)` with speed 0). The Animator sits on the node the file's animation chunk is on (loader_b3d.cpp). Every node is shown after loading, whatever the file said (`insert`). |
| `CreateMesh([parent])` | Makes a mesh with no surfaces | `Mesh(Geometry(), parent)` | fbengine E4 | An empty mesh isn't drawn (an empty box culls it). |
| `CreateCube([parent])` | Makes a cube from -1 to 1 | `Mesh.Cube(parent)` | fbengine E4 | meshutil.cpp `createCube`: one surface, 24 vertices (4 per face, flat normals), 12 triangles; faces in order -Z, +X, +Z, -X, +Y, -Y, each UV 0,0 to 1,1; vertex color white; both UV sets equal. E4's tests match these vertices exactly (converted). |
| `CreateSphere([segments=8][,parent])` | Makes a sphere of radius 1 | `Mesh.Sphere(segments, parent)` | fbengine E4 | `createSphere`: 2 × segments around, segments from pole to pole; each pole is a row of 2 × segments vertices with u at (k + 0.5) / (2 × segments); each ring has 2 × segments + 1 vertices (the seam doubled), u = j / (2 × segments), v = k / segments; normals are the positions. Debug builds take 2 to 100 segments. |
| `CreateCylinder([segments=8][,solid=1][,parent])` | Makes a cylinder of radius 1 from y -1 to 1 | `Mesh.Cylinder(segments, solid, parent)` | fbengine E4 | `createCylinder`: the side is one surface of 2 × (segments + 1) vertices, v 0 at the top; with `solid`, the two caps are a second surface with planar UVs (x / 2 + 0.5, z / 2 + 0.5). 3 to 100 segments. |
| `CreateCone([segments=8][,solid=1][,parent])` | Makes a cone, apex at y 1, base of radius 1 at y -1 | `Mesh.Cone(segments, solid, parent)` | fbengine E4 | `createCone`: the apex is `segments` vertices, and the side's normals are horizontal (x, 0, z), not slanted, so a cone lights like a cylinder; keep it. The base cap is a second surface. 3 to 100 segments. |
| `CopyMesh(mesh[,parent])` | Makes a new mesh with a copy of a mesh's surfaces | `Mesh(mesh.Geometry().Clone(), parent)` | fbengine E4 | Only the surfaces: no position, scale, entity brush, name, children, or collision settings come across (bbblitz3d.cpp `bbCopyMesh`); surfaces with equal brushes merge. |
| `ScaleMesh mesh,x_scale#,y_scale#,z_scale#` | Scales a mesh's vertices | `mesh.Geometry().Scale(Vec3(x, y, z))` | fbengine E4 | Normals go through the cofactor and aren't renormalized (the renderer normalizes, D3D's NORMALIZENORMALS). Changes every copy. |
| `RotateMesh mesh,pitch#,yaw#,roll#` | Turns a mesh's vertices | `mesh.Geometry().Rotate(-Deg(pitch), Deg(yaw), Deg(roll))` | fbengine E4 | Blitz's rotationMatrix, yaw × pitch × roll. |
| `PositionMesh mesh,x#,y#,z#` | Moves a mesh's vertices | `mesh.Geometry().Translate(Vec3(x, y, -z))` | fbengine E4 | |
| `FitMesh mesh,x#,y#,z#,width#,height#,depth#[,uniform=0]` | Scales and moves a mesh's vertices to fill a box | `mesh.Geometry().Fit(box, uniform)` | fbengine E4 | Centers the bounding box in the given one (bbblitz3d.cpp `bbFitMesh`). Uniform picks x's scale when it's strictly the smallest, else y's when it's strictly smallest, else z's, so with x = y < z it uses z's, the largest: keep the quirk. A flat mesh divides by zero. |
| `FlipMesh mesh` | Turns a mesh inside out | `mesh.Geometry().Flip()` | fbengine E4 | Negates normals and swaps each triangle's last two corners. |
| `PaintMesh mesh,brush` | Paints every surface with a brush | `mesh.Geometry().Paint(material.Clone())` | fbengine E4 | Each surface takes a copy; the entity brush is untouched. |
| `AddMesh source_mesh,dest_mesh` | Adds one mesh's surfaces to another's | `dest.Geometry().Add(source.Geometry())` | fbengine E4 | In local coordinates, no transform; each source surface goes into the first dest surface with an equal brush, else a new one; dest takes source's cull box if it has none (meshmodel.cpp `add`). A mesh added to itself is an error. |
| `UpdateNormals mesh` | Recomputes smooth vertex normals | `mesh.Geometry().UpdateNormals()` | fbengine E4 | Per surface: each vertex's normal is the normalized sum of the unit normals of every triangle touching any vertex at the same position within 0.000001 (surface.cpp `updateNormals`), so seams within a surface smooth and across surfaces don't. Degenerate triangles are skipped. |
| `LightMesh mesh,red#,green#,blue#[,range#=0][,x#=0][,y#=0][,z#=0]` | Adds a light's color into a mesh's vertex colors | `mesh.Geometry().AddLight(color, range, Vec3(x, y, -z))` | fbengine E4 | meshutil.cpp `lightMesh`: range 0 adds the color to every vertex; else each vertex facing the point gets color × (range / d) × cos(angle). In mesh space. Adds to what's there, clamps to 0..255, and sets alpha back to 255. Seen only with FX 2. |
| `MeshWidth#(mesh)` | The width of a mesh's bounding box | `mesh.Geometry().Bounds()`, its size's x | fbengine E4 | Local and unscaled. An empty mesh gives -20000000 (geom.h's empty Box); fbengine's AABB of none should give 0, and the migrator need not care. |
| `MeshHeight#(mesh)` | The height of a mesh's bounding box | the Bounds size's y | fbengine E4 | As MeshWidth. |
| `MeshDepth#(mesh)` | The depth of a mesh's bounding box | the Bounds size's z | fbengine E4 | As MeshWidth. |
| `MeshesIntersect(mesh_a,mesh_b)` | Whether two meshes' triangles cross, in world space | `a.Intersects(b)` | fbengine E4 | Triangle edges against triangles (meshcollider.cpp `intersects`): one mesh wholly inside the other, or coplanar overlaps, give False. |
| `CountSurfaces(mesh)` | How many surfaces a mesh has | `mesh.Geometry().SurfaceCount()` | fbengine E4 | |
| `GetSurface(mesh,surface_index)` | A mesh's surface by index | `mesh.Geometry().Surface(index - 1)` | fbengine E4 | 1-based in Blitz. |
| `MeshCullBox mesh,x#,y#,z#,width#,height#,depth#` | Replaces the box a mesh is frustum-culled by | `mesh.Geometry().SetCullBox(box)` | fbengine E4 | Culling only: EntityInView and MeshWidth still use the real box. For skinned meshes, whose bones move vertices out of the box. |

## Surfaces

A surface is one brush's vertices and triangles inside a mesh (surface.h). Its vertex: position,
normal, one ARGB color (bytes), two UV sets, and up to four bone weights. Triangle indices are
16-bit, so a surface holds 65536 vertices at most; fbengine's should be 32-bit (design
question 16).

| Blitz3D | What it does (one short line, from the help and source) | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `CreateSurface(mesh[,brush])` | Adds a surface to a mesh, painted with a copy of the brush | `mesh.Geometry().CreateSurface(material)` | fbengine E4 | No brush means the default white brush. Appended last, so it's surface CountSurfaces. |
| `GetSurfaceBrush(surface)` | A copy of a surface's brush | `surface.Material().Clone()` | fbengine E4 | Blitz makes a new brush each call, to be freed. |
| `FindSurface(mesh,brush)` | The first surface whose brush equals the given one | `mesh.Geometry().FindSurface(material)` | fbengine E4 | Equal means the whole render state compares equal, byte for byte (brush.cpp `operator<`): color, alpha, shininess, blend, FX, and the same texture images and transforms. 0 for none. |
| `ClearSurface surface[,clear_vertices=1][,clear_triangles=1]` | Removes a surface's vertices and/or triangles | `surface.Clear(vertices, triangles)` | fbengine E4 | Clearing vertices but not triangles leaves triangles pointing at nothing until vertices are added. |
| `PaintSurface surface,brush` | Paints a surface with a copy of a brush | `surface.SetMaterial(material.Clone())` | fbengine E4 | |
| `AddVertex(surface,x#,y#,z#[,u#=0][,v#=0][,w#=1])` | Adds a vertex, giving its index | `surface.AddVertex(Vec3(x, y, -z), Vec2(u, v))` | fbengine E4 | Both UV sets get u, v; w is ignored; the color is opaque white; the normal is 0,0,0 (bbblitz3d.cpp `bbAddVertex`), so such a mesh is lit only by ambient light until VertexNormal or UpdateNormals. 0-based. |
| `AddTriangle(surface,v0,v1,v2)` | Adds a triangle, giving its index | `surface.AddTriangle(v0, v2, v1)` | fbengine E4 | Corners swapped for the z flip (Conventions). Blitz's front face is clockwise as seen. Indices aren't checked. |
| `VertexCoords surface,index,x#,y#,z#` | Moves a vertex | `surface.SetVertexPosition(index, Vec3(x, y, -z))` | fbengine E4 | Rebuilds the collision tree the next time it's needed (meshmodel.cpp's geom_changes). |
| `VertexNormal surface,index,nx#,ny#,nz#` | Sets a vertex's normal | `surface.SetVertexNormal(index, Vec3(nx, ny, -nz))` | fbengine E4 | Not normalized; lighting normalizes. |
| `VertexColor surface,index,red#,green#,blue#[,alpha#=1]` | Sets a vertex's color and alpha | `surface.SetVertexColor(index, Color.Srgb(r / 255, g / 255, b / 255, alpha))` | fbengine E4 | Stored as bytes: each channel clamped to 0..255 and truncated, alpha × 255 the same (bbblitz3d.cpp `bbVertexColor`), so fbengine stores a Color32 and reads back exactly. Used only with FX 2. Vertex alpha needs FX 2 for the vertex color to reach the pixels and FX 32 to blend: D3D takes the diffuse color from the vertex only when COLORVERTEX is on, which only FX 2 turns on (gxscene.cpp `setRenderState`), so games use FX 34; the help says 32 alone. |
| `VertexTexCoords surface,index,u#,v#[,w#=1][,coord_set=0]` | Sets one of a vertex's UV sets | `surface.SetVertexUv(index, Vec2(u, v), set)` | fbengine E4 | w ignored; set 0 or 1. |
| `CountVertices(surface)` | How many vertices a surface has | `surface.VertexCount()` | fbengine E4 | |
| `CountTriangles(surface)` | How many triangles a surface has | `surface.TriangleCount()` | fbengine E4 | |
| `VertexX#(surface,index)` | A vertex's x | `surface.VertexPosition(index).x` | fbengine E4 | |
| `VertexY#(surface,index)` | A vertex's y | `surface.VertexPosition(index).y` | fbengine E4 | |
| `VertexZ#(surface,index)` | A vertex's z | `-surface.VertexPosition(index).z` | fbengine E4 | Negated back. |
| `VertexNX#(surface,index)` | A vertex normal's x | `surface.VertexNormal(index).x` | fbengine E4 | |
| `VertexNY#(surface,index)` | A vertex normal's y | `surface.VertexNormal(index).y` | fbengine E4 | |
| `VertexNZ#(surface,index)` | A vertex normal's z | `-surface.VertexNormal(index).z` | fbengine E4 | |
| `VertexRed#(surface,index)` | A vertex's red, 0 to 255 | `surface.VertexColor(index).r`, a Color32's | fbengine E4 | The stored byte, as a Float. |
| `VertexGreen#(surface,index)` | A vertex's green, 0 to 255 | `surface.VertexColor(index).g` | fbengine E4 | |
| `VertexBlue#(surface,index)` | A vertex's blue, 0 to 255 | `surface.VertexColor(index).b` | fbengine E4 | |
| `VertexAlpha#(surface,index)` | A vertex's alpha, 0 to 1 | `surface.VertexColor(index).a / 255` | fbengine E4 | The byte over 255. |
| `VertexU#(surface,index[,coord_set=0])` | A vertex's u in a UV set | `surface.VertexUv(index, set).x` | fbengine E4 | A set other than 0 or 1 is a debug error. |
| `VertexV#(surface,index[,coord_set=0])` | A vertex's v in a UV set | `surface.VertexUv(index, set).y` | fbengine E4 | |
| `VertexW#(surface,index[,coord_set=0])` | A vertex's w | `1.0` | dropped | Always 1 (bbblitz3d.cpp `bbVertexW`); the migrator emits the literal. |
| `TriangleVertex(surface,index,vertex)` | Which vertex is a triangle's corner 0, 1, or 2 | `surface.TriangleVertex(index, corner)` | fbengine E4 | The migrator maps Blitz's corner 1 to 2 and 2 to 1. |

## Cameras

A camera's defaults (camera.cpp's constructor): zoom 1, range 1 to 1000, clear color black,
clearing color and depth, perspective, fog off, fog range 1 to 1000, fog color black, and the 2D
viewport current when it was made (bbblitz3d.cpp `bbCreateCamera`). Cameras render in
EntityOrder's order, highest first, and equal orders in no set order (a priority queue), so
fbengine keeps creation order for ties. Each clears its own viewport per its clear mode, then
draws every mirror's reflection, then the scene (world.cpp `render`).

| Blitz3D | What it does (one short line, from the help and source) | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `CreateCamera([parent])` | Makes a camera | `Camera(parent)` | fbengine E4 | Looks along its own forward axis. The viewport is fixed at creation; a window resized later doesn't change it (design question 10). |
| `CameraZoom camera,zoom#` | Sets the camera's zoom, 1 by default | `camera.SetZoom(zoom)` | fbengine E4 | The near plane's width is 2 × near / zoom (camera.cpp `getFrustum`), so the horizontal field of view is 2 × atan(1 / zoom), 90 degrees at zoom 1, and the vertical one follows the viewport's height over width. Orthographic: the view is 2 × near / zoom world units wide, so it depends on CameraRange's near. |
| `CameraRange camera,near#,far#` | Sets the near and far clip distances | `camera.SetRange(near, far)` | fbengine E4 | Default 1 and 1000. DX7's depth was a 16- or 24-bit buffer; fbengine's 32-bit float depth z-fights less at the same ranges, which is fine. |
| `CameraClsColor camera,red#,green#,blue#` | Sets the color a camera clears its viewport to | `camera.SetClearColor(color)` | fbengine E4 | Cleared with alpha 1. Packed without clamping, as AmbientLight's. |
| `CameraClsMode camera,cls_color,cls_zbuffer` | Whether a camera clears color, depth, both, or neither | `camera.SetClearMode(color, depth)` | fbengine E4 | For overlays: a second camera that doesn't clear color draws over the first. |
| `CameraProjMode camera,mode` | 0 none (not rendered), 1 perspective, 2 orthographic | `camera.SetProjection(Projection.PERSPECTIVE)` | fbengine E4 | Mode 0 skips the camera, clear and all (camera.cpp `beginRenderFrame`). No help page; from the source. |
| `CameraViewport camera,x,y,width,height` | Sets the part of the screen a camera draws to | `camera.SetViewport(x, y, width, height)` | fbengine E4 | In the back buffer's pixels. In fbengine, in the units of E3's 2D coordinates (design question 10). |
| `CameraFogColor camera,red#,green#,blue#` | Sets a camera's fog color, black by default | `camera.SetFogColor(color)` | fbengine E4 (drawn by E5) | |
| `CameraFogRange camera,near#,far#` | Where a camera's fog starts and where it's full, 1 to 1000 by default | `camera.SetFogRange(near, far)` | fbengine E4 (drawn by E5) | Linear in view depth: DX7 used table (pixel) fog on W when the card had it, else vertex fog on eye z (gxscene.cpp's constructor). E5 does per-pixel fog on view-space depth, not radial distance. |
| `CameraFogMode camera,mode` | 0 no fog (default), 1 linear | `camera.SetFogMode(FogMode.LINEAR)` | fbengine E4 (drawn by E5) | Fog applies to the pixel's color before blending with the screen, so additive entities fade toward the fog color too; FX 8 turns fog off per entity. |
| `CameraProject camera,x#,y#,z#` | Projects a world point into the camera's 2D view | `camera.Project(Vec3(x, y, -z))`, a `Vec3?` | fbengine E4 | Blitz (bbblitz3d.cpp `bbCameraProject`): x and y are relative to the viewport's corner, not the screen's; a point behind the camera or past far gives 0,0,0; orthographic always succeeds. fbengine gives window coordinates (the viewport's corner added) and the view depth as z, or Null when the point can't project; `fbengine.blitz`'s CameraProject turns that back into Blitz's globals. |
| `ProjectedX#()` | The last CameraProject's x | `Blitz.ProjectedX()` | fbengine E4 | The Vec3's x less the viewport's x. |
| `ProjectedY#()` | The last CameraProject's y | `Blitz.ProjectedY()` | fbengine E4 | Down from the viewport's top. |
| `ProjectedZ#()` | The last CameraProject's z | `Blitz.ProjectedZ()` | fbengine E4 | Always the camera's near range when the point projected, not its depth (`projected=Vector(...,nr)`); 0 when it didn't. |
| `EntityInView(entity,camera)` | Whether an entity might be in a camera's view | `camera.InView(entity)` | fbengine E4 | A mesh's own bounding box (not its cull box, not its children) against the frustum's six planes, else just its position; True unless all of it is outside one plane (frustum.cpp `cull`), so it can be True for things just off screen. Hidden entities aren't special-cased. |

## Picks

Picking traces a line, or a sphere swept along one, against every shown entity with a pick mode
(world.cpp `traceRay`), and keeps the nearest hit; on a tie, the later entity in hierarchy order.
fbengine returns a `Pick?` Struct of Entity, Point, Normal, Time, Surface, and Triangle.

| Blitz3D | What it does (one short line, from the help and source) | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `EntityPickMode entity,pick_geometry[,obscurer=1]` | 0 not pickable (default), 1 sphere (EntityRadius), 2 polygons, 3 box (EntityBox); and whether it blocks EntityVisible | `entity.SetPickMode(PickMode.POLYGON, obscurer)` | fbengine E4 | Polygons only for meshes, terrains, planes, and BSP; sprites and MD2 have no polygon test (their `collide` gives False). Entities don't obscure until this is called, and this command's default is to obscure. |
| `EntityVisible(src_entity,dest_entity)` | Whether nothing blocks the line between two entities | `source.CanSee(target)` | fbengine E4 | Center to center; only shown entities with a pick mode and obscurer set block, the two ends excepted (world.cpp `checkLOS`). Radius 0. |
| `EntityPick(entity,range#)` | The nearest pickable entity ahead of an entity | `entity.Pick(range)`, a `Pick?` | fbengine E4 | The line runs from the entity's world position along its world forward axis times range, scale included (bbblitz3d.cpp `bbEntityPick` uses `m.k`), so a z-scaled entity picks further. The entity itself isn't excluded. |
| `LinePick(x#,y#,z#,dx#,dy#,dz#[,radius#=0])` | The nearest pickable entity along a line, or a sphere swept along it | `LinePick(Vec3(x, y, -z), Vec3(dx, dy, -dz), radius)`, a `Pick?` | fbengine E4 | dx, dy, dz are the whole line, not a direction; Time is the fraction along it. With a radius, Point is where the sphere touches: its center minus the normal × radius. |
| `CameraPick(camera,viewport_x,viewport_y)` | The nearest pickable entity under a point of a camera's view | `camera.Pick(Vec2(x, y))`, a `Pick?` | fbengine E4 | Blitz's x, y are relative to the viewport's corner (`bbCameraPick` divides by the viewport's size without subtracting its corner); fbengine takes window coordinates, and the migrator adds the corner. Perspective: from the camera's position, not the near plane, to the far plane through the point. Orthographic: from the point on the camera's plane, along forward for far. |
| `PickedX#()` | The last pick's hit point's x | `pick.Point.x`, or `Blitz.PickedX()` | fbengine E4 | Blitz's pick globals aren't cleared by a miss: Time goes back to 1 and Entity to 0, but the point, normal, surface, and triangle keep the last hit's values (bbblitz3d.cpp `doPick` resets only the time). `fbengine.blitz` keeps that. |
| `PickedY#()` | The last pick's hit point's y | `pick.Point.y` | fbengine E4 | |
| `PickedZ#()` | The last pick's hit point's z | `-pick.Point.z` | fbengine E4 | |
| `PickedNX#()` | The last pick's surface normal's x | `pick.Normal.x` | fbengine E4 | |
| `PickedNY#()` | The last pick's surface normal's y | `pick.Normal.y` | fbengine E4 | |
| `PickedNZ#()` | The last pick's surface normal's z | `-pick.Normal.z` | fbengine E4 | |
| `PickedTime#()` | How far along the line the last pick hit, 0 to 1 | `pick.Time` | fbengine E4 | 1 after a miss. |
| `PickedEntity()` | The last pick's entity, 0 for a miss | `pick.Entity` | fbengine E4 | |
| `PickedSurface()` | The surface the last pick hit | `pick.Surface`, a `Surface?` | fbengine E4 | Set only by polygon hits on meshes; BSP hits set 0 and triangle 0 (q3bsprep.cpp's collision triangles); sphere, box, terrain, and plane hits leave the last one there. |
| `PickedTriangle()` | The triangle the last pick hit, in its surface | `pick.Triangle` | fbengine E4 | As PickedSurface; before any polygon hit it's 65535 (collision.h's index ~0 in an unsigned short). fbengine gives -1 for none, and `fbengine.blitz` 65535. |

## Lights

Lights are DX7's (gxlight.cpp): at most eight a frame, the first eight shown lights in hierarchy
order (gxscene.cpp `begin`); the rest light nothing. Each lights every entity. A light's diffuse
color is LightColor; its specular color is always white.

| Blitz3D | What it does (one short line, from the help and source) | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `CreateLight([type=1][,parent])` | Makes a directional (1), point (2), or spot (3) light | `Light(LightType.DIRECTIONAL, parent)` | fbengine E4 (lit by E5) | Shines along its forward axis (light.cpp `beginRender` uses `m.k`), the same direction after conversion. White, range 1000, cone 0 to 90 degrees. Any other type number is directional. |
| `LightColor light,red#,green#,blue#` | Sets a light's color | `light.SetColor(color)` | fbengine E4 (lit by E5) | Unclamped: negative colors darken, the known trick, and E5 keeps them. |
| `LightRange light,range#` | Sets how a point or spot light falls off, 1000 by default | `light.SetRange(range)` | fbengine E4 (lit by E5) | Not a cutoff: attenuation is 1 / (d / range) = range / d, D3D's linear term alone, with D3D's own range at its maximum (gxlight.cpp `setRange`), so a light is brighter than its color nearer than `range` (clamped later) and never quite reaches 0. |
| `LightConeAngles light,inner_angle#,outer_angle#` | Sets a spot light's cone, 0 and 90 by default | `light.SetConeAngles(Deg(inner), Deg(outer))` | fbengine E4 (lit by E5) | Whole apex angles, D3D's theta and phi, so 90 is 45 degrees off the axis. Clamped: inner to 0..180, outer to inner..180 (bbblitz3d.cpp). Falloff 1: between the cones the light is (cos a - cos(phi / 2)) / (cos(theta / 2) - cos(phi / 2)). |

## Pivots, mirrors, and planes

| Blitz3D | What it does (one short line, from the help and source) | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `CreatePivot([parent])` | Makes an invisible entity to move, turn, and parent things to | `Pivot(parent)` | fbengine E4 | |
| `CreateMirror([parent])` | Makes an infinite mirror at its y = 0 plane, reflecting everything | `Mirror(parent)` | fbengine E4 (drawn by E5) | For each camera, before the normal view, the whole scene is drawn reflected through the mirror's local XZ plane (`t * scale(1,-1,1) * -t * camera`, world.cpp `render`) with culling flipped, into the same viewport and depth buffer. No clip plane or stencil: things below the mirror reflect above it, and the floor, drawn after, covers the reflection unless it's translucent (the help's alpha 0.5 plane). |
| `CreatePlane([segments=1][,parent])` | Makes an infinite flat ground at its y = 0, facing up | `GroundPlane(segments, parent)` | fbengine E4 | Built each frame from the camera's far plane projected onto y = 0 in a segments × segments grid (planemodel.cpp `render`), clipped at the horizon; UVs are the plane's local x and z, one repeat a unit; not drawn when the camera is below it. 1 to 20 segments by the debug check, but its static vertex grid is 17 × 17 (`vts[17][17]`), so 17 to 20 overflow it: the help's 16 is the real limit, and fbengine panics above it. Collisions and picks: the infinite plane, from above only. |

## Sprites

| Blitz3D | What it does (one short line, from the help and source) | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `CreateSprite([parent])` | Makes a sprite: a 2 × 2 quad that faces the camera | `Sprite(parent)` | fbengine E4 | FX 1 (full-bright) from the start, blend auto, view mode 1 (bbblitz3d.cpp `bbCreateSprite`). The quad is -1..1 in its x and y, UV 0,0 at its top-left; its normals are 0,0,0, so with FX 0 only ambient light lights it. Drawn in world space, frustum-culled. |
| `LoadSprite(file$[,texture_flags=1][,parent])` | Makes a sprite with a loaded texture | `Sprite.Load(path, options, parent)` | fbengine E4 | The blend comes from the flags (`bbLoadSprite`): masked (4) → replace with alpha test, alpha (2) → alpha, else add. So a plain image loads additive, and the migrator sets `Blend.ADD` explicitly. |
| `RotateSprite sprite,angle#` | Turns a sprite in its plane, in degrees | `sprite.SetSpriteRotation(Deg(angle))` | fbengine E4 | Counterclockwise as seen (`rollMatrix`), on top of the view mode's orientation; roll keeps its sign across the z flip. |
| `ScaleSprite sprite,x_scale#,y_scale#` | Sets a sprite's half-width and half-height, 1,1 by default | `sprite.SetSpriteScale(Vec2(x, y))` | fbengine E4 | Separate from ScaleEntity, which view modes 1 and 3 ignore (they replace or orthonormalize the entity's matrix, sprite.cpp `render`). |
| `HandleSprite sprite,x_handle#,y_handle#` | Sets which point of the quad sits at the entity's position | `sprite.SetHandle(Vec2(x, y))` | fbengine E4 | In the quad's -1..1 units, y up: 0,-1 puts the bottom middle at the position. |
| `SpriteViewMode sprite,view_mode` | How a sprite turns toward the camera | `sprite.SetViewMode(SpriteViewMode.BILLBOARD)` | fbengine E4 | From sprite.cpp `render`, whose comments and the help name modes 1 and 2 differently: 1 takes the camera's whole rotation (parallel to the screen, ignoring the entity's rotation and scale; BILLBOARD); 2 keeps the entity's own (FIXED); 3 the camera's forward with the entity's up, orthonormalized, scale dropped (UPRIGHT); 4 the camera's yaw times the entity's own matrix (UPRIGHT_YAW, for trees). Mode 2 is one-sided unless FX 16. |

## MD2 models

| Blitz3D | What it does (one short line, from the help and source) | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `LoadMD2(file$[,parent])` | Loads a Quake 2 MD2 model | `Md2Model.Load(path, parent)` | fbengine E4 | Axes swizzled to y, z, x from Quake's (md2rep.cpp), then converted. Normals from MD2's table. No texture is loaded: games EntityTexture one. 0 for a file with no frames. Polygon picks and collisions don't hit it. |
| `AnimateMD2 md2[,mode=1][,speed#=1][,first_frame=0][,last_frame=9999][,transition#=0]` | Plays a range of an MD2's frames | `md2.AnimateFrames(first, last, AnimMode.LOOP, speed, transition)` | fbengine E7 | md2model.cpp `startMD2Anim`: first and last swap if reversed and clamp to the frames; looping or positive speeds start at first, else at last; looping blends frame last - 1 into first and never shows `last` itself. The transition goes from the current pose to the start at 1/transition per UpdateWorld call, not scaled by elapsed (`trans_time+=trans_speed`). Mode 0 freezes it. |
| `MD2AnimTime#(md2)` | The current frame, as a Float | `md2.FrameTime()` | fbengine E7 | |
| `MD2AnimLength(md2)` | How many frames the MD2 has | `md2.FrameCount()` | fbengine E7 | All of the file's frames, not the playing range's length. |
| `MD2Animating(md2)` | Whether it's animating or in a transition | `md2.FramesAnimating()` | fbengine E7 | |

## BSP models

| Blitz3D | What it does (one short line, from the help and source) | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `LoadBSP(file$[,gamma_adj#=0][,parent])` | Loads a Quake 3 BSP level, with its lightmaps, patches, and visibility | `BspModel.Load(path, gamma, parent)` | fbengine E4 | Version 0x2e only. Axes (-y, z, x) from Quake's (q3bsprep.cpp `tf`), then converted. Lightmaps go through pow(x, 1 - gamma_adj). Drawn by its PVS clusters; collides as polygons. Q3's shaders aren't read: textures by name. |
| `BSPLighting bsp,use_lightmaps` | Lightmaps (default) or vertex lighting | `bsp.SetLightmaps(useLightmaps)` | fbengine E4 (drawn by E5) | With lightmaps, a face's layer 0 is its lightmap and layer 1 its texture (multiply); faces without one, and every face with lightmaps off, use their vertex colors as emissive light (FX_EMISSIVE, gxscene.cpp's "Q3 hack"). Either way FX_CONDLIGHT: directional lights don't light it, point and spot lights do. |
| `BSPAmbientLight bsp,red#,green#,blue#` | A BSP's own ambient light, black by default | `bsp.SetAmbientLight(color)` | fbengine E4 (lit by E5) | Replaces AmbientLight while the BSP draws (gxscene.cpp `setAmbient2`). |

## Terrain

A terrain is a size × size grid, a power of two, from 0 to size in its local x and z, with
heights from 0 to 1 in y; ScaleEntity sizes it. Heights are bytes (h × 255, truncated) and wrap:
the grid is indexed through a mask (terrainrep.cpp `getHeight`), so the row and column at `size`
are row and column 0's. It's drawn with ROAM-style LOD to a triangle budget, and collides with
the whole grid.

| Blitz3D | What it does (one short line, from the help and source) | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `CreateTerrain(grid_size[,parent])` | Makes a flat terrain of grid_size × grid_size cells | `Terrain(size, parent)` | fbengine E4 | A size that isn't a power of two is an error even in release builds. Defaults: detail 2000 triangles, no morphing, shading off. UVs are (x, size - z) in cells (terrainrep.cpp `render`): a texture repeats every cell until it's ScaleTextured by the size. |
| `LoadTerrain(heightmap_file$[,parent])` | Makes a terrain from a square, power-of-two heightmap image | `Terrain.Load(path, parent)` | fbengine E4 | Height = max(r, g, b) / 255, and the image's top row is the terrain's far row, z = size - 1 (bbblitz3d.cpp `bbLoadTerrain`). A missing, non-square, or non-power-of-two image is an error. |
| `TerrainDetail terrain,detail_level[,morph=0]` | How many triangles a terrain is drawn with, and whether vertices morph | `terrain.SetDetail(triangles, morph)` | fbengine E4 | The LOD changes only drawing; collisions and TerrainY ignore it. E4 can port terrainrep.cpp's ROAM, or use another LOD that looks the same at 2000 triangles. |
| `TerrainShading terrain,enable` | Whether a terrain gets real normals | `terrain.SetShaded(enable)` | fbengine E4 (lit by E5) | Off (default): every normal points up, so lights shade it flatly. On: each vertex's normal averages its four neighbors' planes (`getNormal`). |
| `TerrainX#(terrain,world_x#,world_y#,world_z#)` | The world x of the terrain's surface under a point | `terrain.SurfacePoint(Vec3(x, y, -z)).x` | fbengine E4 | The three are one point: world to terrain space, height by bilinear interpolation of the cell's four corners (not the drawn triangles), back to world (`terrainVector`). Not clamped: outside the grid the heights wrap. |
| `TerrainY#(terrain,world_x#,world_y#,world_z#)` | The world height of the terrain under a point | `terrain.SurfacePoint(point).y` | fbengine E4 | The usual "stand on the ground" call. |
| `TerrainZ#(terrain,world_x#,world_y#,world_z#)` | The world z of the terrain's surface under a point | `-terrain.SurfacePoint(point).z` | fbengine E4 | |
| `TerrainSize(terrain)` | The terrain's grid size | `terrain.Size()` | fbengine E4 | |
| `TerrainHeight#(terrain,terrain_x,terrain_z)` | A grid point's height, 0 to 1 | `terrain.Height(x, z)` | fbengine E4 | In grid units, unscaled. Outside 0..size gives 0; at size it's 0's (the wrap). Reads back the byte / 255. Grid z is design question 1's: fbengine's grid runs along -Z, so Blitz's grid z = k is fbengine's k and the migrator passes grid coordinates as they are. |
| `ModifyTerrain terrain,terrain_x,terrain_z,height#[,realtime=0]` | Sets a grid point's height | `terrain.SetHeight(x, z, height, realtime)` | fbengine E4 | Truncated to a byte; above 1 overflows the byte (undefined in C++), so fbengine clamps. Without realtime, the LOD's error tree is rebuilt at the next render: slow for one change a frame, the same result. |

## The scene's audio

| Blitz3D | What it does (one short line, from the help and source) | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `CreateListener(parent[,rolloff_factor#=1][,doppler_scale#=1][,distance_scale#=1])` | Makes the one 3D sound listener, carried by an entity | `Listener.Attach(camera, rolloff, doppler, distanceScale)` | fbengine_audio E9 | One only (a second is a debug error). Its position, velocity, forward, and up are set at each RenderWorld (listener.cpp `renderListener`), not UpdateWorld; the velocity is UpdateWorld's. |
| `EmitSound(sound,entity)` | Plays a 3D sound at an entity, following it | `sound.EmitFrom(entity)`, a `Channel` | fbengine_audio E9 | The channel's position and velocity follow the entity at each RenderWorld until it stops (object.cpp `updateSounds`). Needs a listener (a debug error). How fbengine_audio reaches entities is design question 20. |

## Entities: movement

An entity stores a local position, rotation (a unit quaternion), and scale; its matrix is
rotation × scale per axis, then the position, and its world matrix is the parent's times it
(entity.cpp `getLocalTform`, `getWorldTform`). Scale is per axis in the entity's own frame, so a
non-uniformly scaled parent with a rotated child shears it, as any matrix hierarchy does. The
world rotation and scale Blitz reports are products of the locals, not decompositions of the
world matrix (`getWorldRotation`, `getWorldScale`), which differ under a sheared parent; fbengine
keeps both definitions.

| Blitz3D | What it does (one short line, from the help and source) | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `MoveEntity entity,x#,y#,z#` | Moves an entity along its own axes | `entity.Move(Vec3(x, y, -z))` | fbengine E4 | position += local rotation × v (bbblitz3d.cpp `bbMoveEntity`): the entity's rotation but not its scale, in the parent's units. MoveEntity 0,0,1 is one step forward in both engines after conversion. |
| `TurnEntity entity,pitch#,yaw#,roll#[,global=0]` | Turns an entity by Euler angles | `entity.Turn(-Deg(pitch), Deg(yaw), Deg(roll), space)` | fbengine E4 | Local: rotation = rotation × R(p, y, r), about its own axes; global: world rotation = R × world rotation, about the world's axes. R is yaw × pitch × roll. A Quat overload `Turn(rotation, space)` too. |
| `TranslateEntity entity,x#,y#,z#[,global=0]` | Moves an entity along its parent's (or the world's) axes | `entity.Translate(Vec3(x, y, -z), space)` | fbengine E4 | Local adds to the local position, ignoring the entity's rotation; global adds to the world position. |
| `PositionEntity entity,x#,y#,z#[,global=0]` | Puts an entity at a position | `entity.SetPosition(Vec3(x, y, -z), space)` | fbengine E4 | Global goes through the parent's whole inverse world matrix. Not a teleport for collisions: the next UpdateWorld sweeps from the old position unless ResetEntity follows. |
| `ScaleEntity entity,x_scale#,y_scale#,z_scale#[,global=0]` | Sets an entity's scale | `entity.SetScale(Vec3(x, y, z), space)` | fbengine E4 | No negation: scale is a magnitude per axis. Global divides by the parent's world scale per component (`setWorldScale`), ignoring rotation. Collision radii and boxes don't scale with it; polygon collisions and picks do. |
| `RotateEntity entity,pitch#,yaw#,roll#[,global=0]` | Sets an entity's rotation by Euler angles | `entity.SetRotation(-Deg(pitch), Deg(yaw), Deg(roll), space)` | fbengine E4 | Global sets the world rotation through the parent's world rotation (scale ignored). A Quat overload too. |
| `PointEntity entity,target[,roll#=0]` | Turns an entity to face another | `entity.PointAt(target, Deg(roll))` | fbengine E4 | Sets the world rotation to the pitch and yaw of the line between their world positions, with this roll (`bbPointEntity`); at the same position that's pitch 0, yaw 0. A `PointAt(point: Vec3, roll)` overload is new. |
| `AlignToVector entity,vector_x#,vector_y#,vector_z#,axis[,rate#=1]` | Turns one of an entity's axes toward a direction, by a fraction of the angle | `entity.AlignToVector(Vec3(x, y, -z), Axis.FORWARD, rate)` | fbengine E4 | Axis 1 X (RIGHT), 2 Y (UP), anything else Z (FORWARD). Turns the world rotation about axis × v by rate × the angle (`bbAlignToVector`); does nothing for a vector shorter than 0.000001 or one already within 0.000001 of the axis; for the opposite direction it turns rate × 180 degrees about the next axis (Y for X, Z for Y, X for Z). Mirror-symmetric except that last turn's sense, which the port mirrors. The one entity command without a debug check. |

## Entities: where they are

| Blitz3D | What it does (one short line, from the help and source) | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `EntityX#(entity[,global=0])` | An entity's x | `entity.Position(space).x` | fbengine E4 | Local (the parent's space) by default, which surprises people with parented entities. |
| `EntityY#(entity[,global=0])` | An entity's y | `entity.Position(space).y` | fbengine E4 | |
| `EntityZ#(entity[,global=0])` | An entity's z | `-entity.Position(space).z` | fbengine E4 | |
| `EntityPitch#(entity[,global=0])` | An entity's pitch, -90 to 90 | `-ToDegrees(entity.Pitch(space))` | fbengine E4 | From the forward axis only: -atan2(k.y, sqrt(k.x² + k.z²)) (geom.h `quatPitch`). fbengine's Pitch, Yaw, and Roll are these formulas in its space, so a Blitz round trip holds: forward f = (k.x, k.y, -k.z), pitch = atan2(f.y, sqrt(f.x² + f.z²)). |
| `EntityYaw#(entity[,global=0])` | An entity's yaw, -180 to 180 | `ToDegrees(entity.Yaw(space))` | fbengine E4 | -atan2(k.x, k.z) (`quatYaw`), in fbengine atan2(-f.x, -f.z). Near pitch ±90 it's whatever the forward axis's tiny x and z say: gimbal noise, as in Blitz. |
| `EntityRoll#(entity[,global=0])` | An entity's roll, -180 to 180 | `ToDegrees(entity.Roll(space))` | fbengine E4 | atan2(i.y, j.y) (geom.h `matrixRoll`), the same in both spaces. |
| `GetMatElement#(entity,row,column)` | One element of an entity's world matrix | `entity.WorldMatrix()[row, column]` | fbengine E4 | Rows 0 to 2 are the world matrix's i, j, k axes with scale, row 3 the position; columns 0 to 2 their x, y, z (`bbGetMatElement`). Signs per Conventions; `Blitz.MatElement` does it. |
| `TFormPoint x#,y#,z#,source_entity,dest_entity` | Transforms a point from one entity's space to another's | `TransformPoint(Vec3(x, y, -z), source, target)` | fbengine E4 | Either entity can be 0, the world (`bbTFormPoint`). Gives the Vec3; the migrator keeps Blitz's TFormed globals. |
| `TFormVector x#,y#,z#,source_entity,dest_entity` | Transforms a direction (no translation) | `TransformVector(Vec3(x, y, -z), source, target)` | fbengine E4 | Rotation and scale, not normalized. |
| `TFormNormal x#,y#,z#,source_entity,dest_entity` | Transforms a normal, normalized | `TransformNormal(Vec3(x, y, -z), source, target)` | fbengine E4 | Through each matrix's cofactor (its inverse transpose times its determinant), then normalized; a zero vector gives NaNs. |
| `TFormedX#()` | The last TForm's x | `Blitz.TFormedX()` | fbengine E4 | The migrator stores the last TransformPoint, Vector, or Normal. |
| `TFormedY#()` | The last TForm's y | `Blitz.TFormedY()` | fbengine E4 | |
| `TFormedZ#()` | The last TForm's z | `Blitz.TFormedZ()` | fbengine E4 | Negated back. |
| `VectorYaw#(x#,y#,z#)` | The yaw a direction points at | `ToDegrees(VectorYaw(Vec3(x, y, -z)))` | fbengine E4 | -atan2(x, z) in Blitz; 0 for a zero vector. |
| `VectorPitch#(x#,y#,z#)` | The pitch a direction points at | `-ToDegrees(VectorPitch(Vec3(x, y, -z)))` | fbengine E4 | -atan2(y, sqrt(x² + z²)) in Blitz: positive when it points down. |
| `DeltaPitch#(src_entity,dest_entity)` | How far an entity would pitch to face another | `-ToDegrees(source.DeltaPitch(target))` | fbengine E4 | The pitch of the line to dest minus the pitch of src's forward axis, wrapped to -180..180 (`bbDeltaPitch`). |
| `DeltaYaw#(src_entity,dest_entity)` | How far an entity would turn to face another | `ToDegrees(source.DeltaYaw(target))` | fbengine E4 | The same for yaw, wrapped into [-180, 180). Ignores the line's pitch. |
| `EntityDistance#(source_entity,destination_entity)` | The distance between two entities' world positions | `source.DistanceTo(target)` | fbengine E4 | |

## Entities: the hierarchy and their state

Children are kept in the order they were parented, each new one last, and top-level entities in
creation order (entity.cpp `insert`). Every traversal (collisions, lights' eight, rendering, picks)
walks that tree depth first, parent before children, so that order is behavior (see Behaviors).

| Blitz3D | What it does (one short line, from the help and source) | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `CopyEntity(entity[,parent])` | Copies an entity and its children | `entity.Copy(parent)` | fbengine E4 | object.cpp `copy`: meshes share their geometry, entity brushes and collision and pick settings, order, and name copy, animators copy with their keys (not animating), and every copy is shown, even of hidden entities (`insert`). Cameras, lights, and terrains have no clone of their own, so their copies are plain Objects that act as pivots (EntityClass "Pivot"); fbengine copies them properly (design question 14). The copy keeps the original's local transform under its new parent. |
| `EntityParent entity,parent[,global=1]` | Gives an entity a new parent, or none | `entity.SetParent(parent, keepWorld)` | fbengine E4 | global = keepWorld = True by default: the world transform stays and the local one changes; False keeps the local one, so it jumps, and resets its collisions (`bbEntityParent`). The same parent again does nothing, not even moving it last among its siblings. A parent that's itself or a descendant is a debug error; fbengine panics. |
| `GetParent(entity)` | An entity's parent, 0 for none | `entity.Parent()`, an `Entity?` | fbengine E4 | |
| `CountChildren(entity)` | How many children an entity has | `entity.ChildCount()` | fbengine E4 | Direct children only. |
| `GetChild(entity,index)` | An entity's child by index | `entity.Child(index - 1)` | fbengine E4 | 1-based in Blitz, in parenting order; out of range gives 0. `entity.Children()` is new. |
| `FindChild(entity,name$)` | The first entity of a name below an entity | `entity.FindChild(name)`, an `Entity?` | fbengine E4 | Depth first, parent before children, and the entity itself is checked first (bbblitz3d.cpp `findChild`), so FindChild(e, EntityName(e)) is e. Case-sensitive. |
| `HideEntity entity` | Hides an entity and its children | `entity.Hide()` | fbengine E4 | Hidden means not drawn, not lit by (a light), not rendering (a camera), not animated, not collided with or colliding, and not picked (`setVisible`, `setEnabled`, world.cpp's enumVisible and enumEnabled skip the whole branch). Its children keep their own flags. |
| `ShowEntity entity` | Shows an entity again | `entity.Show()` | fbengine E4 | Resets its collisions (no sweep from where it was hidden). A child shown under a hidden parent stays hidden. `entity.Hidden()` is new. |
| `FreeEntity entity` | Frees an entity and its children | `entity.Free()` | fbengine E4 | Children are freed too; their handles go stale. 0 does nothing. Sounds it emits stop following it. |
| `NameEntity entity,name$` | Names an entity | `entity.SetName(name)` | fbengine E4 | Files' node names come in as names; FindChild and LoadAnimSeq match them. |
| `EntityName$(entity)` | An entity's name | `entity.Name()` | fbengine E4 | |
| `EntityClass$(entity)` | What kind of entity it is, as a word | `Blitz.EntityClass(entity)`, from `entity.Kind()` | fbengine E4 | Blitz's words (`bbEntityClass`): "Pivot", "Light", "Camera", "Mirror", "Listener", "Sprite", "Terrain", "Plane", "Mesh", "MD2", "BSP". fbengine's `EntityKind` enum is PIVOT, LIGHT, CAMERA, MIRROR, LISTENER, SPRITE, TERRAIN, PLANE, MESH, MD2, BSP. |

## Entities: how they look

These set the entity's own brush, on any Model. At render time each surface draws with its brush
combined with the entity's (brush.cpp `Brush(a, b)`): colors multiply, alphas multiply,
shininesses add, the entity's blend wins when it isn't 0, FX flags OR, and the entity's texture
on a layer replaces the surface's there, other layers staying the surface's.

| Blitz3D | What it does (one short line, from the help and source) | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `PaintEntity entity,brush` | Sets an entity's whole brush to a copy of one | `model.SetMaterial(material.Clone())` | fbengine E4 | Color, alpha, shininess, blend, FX, and textures all at once; the surfaces' brushes stay. |
| `EntityColor entity,red#,green#,blue#` | Sets an entity's color | `model.SetColor(color)` | fbengine E4 (drawn by E5) | Multiplies the surfaces' colors, white by default, rather than replacing them. FX 2 ignores it: vertex colors become the material color. |
| `EntityAlpha entity,alpha#` | Sets an entity's alpha, 0 to 1 | `model.SetAlpha(alpha)` | fbengine E4 (drawn by E5) | Multiplies the surfaces'. 0 isn't drawn at all but still collides and picks (model.cpp `beginRender`); below 1 blends and sorts. Tweened by RenderWorld. |
| `EntityShininess entity,shininess#` | Sets an entity's shininess, 0 to 1 | `model.SetShininess(shininess)` | fbengine E4 (lit by E5) | Adds to the surfaces'. |
| `EntityTexture entity,texture[,frame=0][,index=0]` | Puts a texture's frame on one of an entity's layers | `model.SetTexture(texture, frame, layer)` | fbengine E4 (drawn by E5) | Replaces the surfaces' texture on that layer only. |
| `EntityBlend entity,blend` | How an entity blends with the screen | `model.SetBlend(Blend.ADD)` | fbengine E4 (drawn by E5) | 0 auto (the default; the help says 1), 1 alpha (src alpha, 1 - src alpha), 2 multiply (dest × src, alpha ignored), 3 add (src × src alpha + dest) (gxscene.cpp `setRenderState`). Auto and 1 resolve by brush.cpp `getBlend`: alpha when one layer is transparent, or FX 32, or alpha below 1; 1 with nothing transparent is opaque. Anything not opaque goes in the sorted transparent pass. |
| `EntityFX entity,fx` | Sets an entity's effect flags | `model.SetEffects(Effects(fullBright: True, vertexColors: True))` | fbengine E4 (drawn by E5) | 1 full-bright (no lights, ambient white: exactly the color), 2 vertex colors as the material's diffuse and ambient color, 4 flat shading, 8 no fog, 16 no back-face culling, 32 forced alpha blending (gxscene.cpp). OR'd with the surfaces' FX, so an entity can't turn a surface's off. Internal flags 0x2000 alpha test, 0x4000 conditional lighting, 0x8000 emissive aren't for games. |
| `EntityAutoFade entity,near#,far#` | Fades an entity out between two distances from the camera | `model.SetAutoFade(near, far)` | fbengine E4 (drawn by E5) | From the camera's position to the entity's origin (the mirrored camera's in a mirror pass): past far it isn't drawn; between, alpha × (1 - (d - near) / (far - near)) (model.cpp `doAutoFade`), which makes it alpha-blended. |
| `EntityOrder entity,order` | Draws an entity before (positive) or after (negative) everything else, without the depth buffer; orders cameras | `model.SetOrder(order)`, `camera.SetOrder(order)` | fbengine E4 (drawn by E5) | Only for models and cameras. World.cpp `render`: positive orders first, highest first, then the normal scene, then negative orders, -1 before -2, so the most negative is on top; ordered ones neither test nor write depth, and each one's transparent parts draw right after it. Affects the entity, not its children. Cameras: highest renders first. |
| `GetEntityBrush(entity)` | A copy of an entity's own brush | `model.Material().Clone()` | fbengine E4 | The entity's brush only, not its surfaces'. Blitz makes a new brush to free. |

## Collisions

Blitz's collisions are swept ellipsoids against spheres, polygons, and boxes, run in UpdateWorld
(world.cpp `collide`, collision.cpp). fbengine ports them as they are (design question 7).

| Blitz3D | What it does (one short line, from the help and source) | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `Collisions src_type,dest_type,method,response` | Turns on collisions from one type to another, by a method and a response | `Collisions.Enable(source, target, CollisionMethod.POLYGON, CollisionResponse.SLIDE)` | fbengine E4 | Methods: 1 SPHERE (ellipsoid to the target's EntityRadius x as a sphere), 2 POLYGON, 3 BOX (EntityBox). Responses: 1 STOP, 2 SLIDE, 3 SLIDE_XZ (the slide's length limited by the move's horizontal length, so gravity alone doesn't slide it down slopes and walking up one keeps its speed; the help's "prevent sliding down slopes"). A pair already enabled is ignored, even with another method or response (world.cpp `addCollision`): only ClearCollisions changes it. Response 0 isn't documented but slides without a limit. Types 0 to 999. |
| `ClearCollisions` | Turns off every collision pair | `Collisions.Clear()` | fbengine E4 | |
| `EntityType entity,collision_type[,recursive=0]` | Sets an entity's collision type, 0 for none | `entity.SetCollisionType(type, recursive)` | fbengine E4 | 0 to 999 (a debug error outside). Resets its collisions (no sweep this time), and recursive sets and resets every descendant too. |
| `GetEntityType(entity)` | An entity's collision type | `entity.CollisionType()` | fbengine E4 | |
| `EntityRadius entity,x_radius#[,y_radius#]` | Sets an entity's collision ellipsoid, or sphere | `entity.SetCollisionRadius(horizontal, vertical)` | fbengine E4 | Radii (x, y, x): x for both x and z, y 0 or omitted meaning x (`bbEntityRadius`). Default 1, 1, 1 (object.cpp). Not scaled by ScaleEntity. A one-argument overload for spheres. |
| `EntityBox entity,x#,y#,z#,width#,height#,depth#` | Sets an entity's collision and pick box | `entity.SetCollisionBox(box)` | fbengine E4 | In the entity's space, moved and turned with it but not scaled (world.cpp `hitTest` normalizes the axes). Default -1..1 (object.cpp). Converted per Conventions. |
| `ResetEntity entity` | Forgets an entity's collisions and where it was, so its next move isn't swept | `entity.ResetCollisions()` | fbengine E4 | Clears the list, zeroes the velocity, and sets the previous position to the current one (object.cpp `reset`): the way to teleport. |
| `EntityCollided(entity,type)` | The first entity of a type it collided with in the last UpdateWorld | `entity.Collided(type)`, an `Entity?` | fbengine E4 | Searches the entity's list in order. Targets get entries too, but see Behaviors: a target processed after the source loses them. |
| `CountCollisions(entity)` | How many collisions it had in the last UpdateWorld | `entity.Collisions().Count` | fbengine E4 | `Collisions()` gives `Collision[]`, valid until the next UpdateWorld. At most 9 of its own as a source per update (the 10th hit is the panic, and isn't recorded). |
| `CollisionX#(entity,collision_index)` | Where a collision touched, x | `entity.Collisions()[index - 1].Point.x` | fbengine E4 | 1-based in Blitz. The point on the target: the ellipsoid's center at the hit minus the normal × the x radius, its y then unsquashed (world.cpp `collided`). World space. |
| `CollisionY#(entity,collision_index)` | Where a collision touched, y | `...Point.y` | fbengine E4 | |
| `CollisionZ#(entity,collision_index)` | Where a collision touched, z | `-...Point.z` | fbengine E4 | |
| `CollisionNX#(entity,collision_index)` | A collision's normal, x | `...Normal.x` | fbengine E4 | For an ellipsoid source the normal is in the squashed space (y scaled by x radius / y radius) and isn't corrected back: keep it. |
| `CollisionNY#(entity,collision_index)` | A collision's normal, y | `...Normal.y` | fbengine E4 | Games test NY > 0.7 or so for "on the ground": it must match. |
| `CollisionNZ#(entity,collision_index)` | A collision's normal, z | `-...Normal.z` | fbengine E4 | |
| `CollisionTime#(entity,collision_index)` | When in the move a collision happened, 0 to 1 | `...Time` | fbengine E4 | The fraction of that iteration's line, and after a first slide the line is the slide's, not the original move's. |
| `CollisionEntity(entity,collision_index)` | The other entity of a collision | `...Entity` | fbengine E4 | |
| `CollisionSurface(entity,collision_index)` | The surface a collision hit | `...Surface`, a `Surface?` | fbengine E4 | Polygon hits on meshes only; 0 for BSP (whose triangle is then 0), terrain, plane, sphere, and box hits. |
| `CollisionTriangle(entity,collision_index)` | The triangle a collision hit, in its surface | `...Triangle` | fbengine E4 | 65535 when there's none (collision.h); fbengine -1, `fbengine.blitz` 65535. |

## Animation

Blitz's keyframe animation (animator.cpp, animation.cpp): an Animator on the root of a loaded or
built hierarchy holds sequences, each with integer-frame keys per object for position, rotation,
and scale, interpolated linearly (position, scale) and by slerp (rotation, shortest way), and
held at the first and last keys outside them. Time is in frames, advanced by speed × UpdateWorld's
elapsed. fbengine's E7 keeps these commands as Methods on Entity over its own clips.

| Blitz3D | What it does (one short line, from the help and source) | FutureBlitz | Where | Notes |
|---|---|---|---|---|
| `LoadAnimSeq(entity,file$)` | Adds a file's animation to an entity as more sequences | `entity.LoadAnimSequence(path)` | fbengine E7 | Loads the file for its animation only and appends its sequences, matching objects by name; objects it lacks get no keys (animator.cpp `addSeqs`). Gives the last sequence's index, even when the file didn't load (then the existing last one), or -1 when the entity has no Animator. |
| `SetAnimTime entity,time#[,anim_seq=0]` | Poses an entity at a time in a sequence, and stops animating | `entity.SetAnimTime(time, sequence)` | fbengine E7 | The time wraps into 0..length (fmod, negatives up). No Animator is an error in release builds too ("Entity has not animation", sic). A sequence equal to the count reads past the end (an off-by-one, `seq>_seqs.size()`); fbengine panics. |
| `Animate entity[,mode=1][,speed#=1][,sequence=0][,transition#=0]` | Plays a sequence: 0 stop, 1 loop, 2 ping-pong, 3 once | `entity.Animate(AnimMode.LOOP, speed, sequence, transition)` | fbengine E7 | Each call restarts at time 0 (or the end for a negative speed), so calling it every frame freezes the first frame; games check AnimSeq or Animating first. Mode 0 with the default speed 1 jumps to frame 0 and stops; only Animate e, 0, 0 stops where it is (animator.cpp `animate`). Transition: a lerp and slerp from the current pose to the start pose over `transition` frames of elapsed, during which time doesn't advance. Ping-pong flips the speed at each end; once stops at the end and Animating goes False. No Animator is an error in release builds. |
| `SetAnimKey entity,frame[,pos_key=1][,rot_key=1][,scale_key=1]` | Records an entity's current local transform as a key at a frame | `entity.SetAnimKey(frame, position, rotation, scale)` | fbengine E7 | The key goes in the entity's own pending keys; they only play once AddAnimSeq makes a sequence of them (the help's "must have a sequence" isn't checked). |
| `AddAnimSeq(entity,length)` | Makes a sequence of `length` frames from the keys SetAnimKey recorded | `entity.AddAnimSequence(length)` | fbengine E7 | With no Animator, it makes one over the entity and all its descendants and takes their pending keys as sequence 0 (animator.cpp's constructor); else it appends one. Gives its index. |
| `ExtractAnimSeq(entity,first_frame,last_frame[,anim_seq=0])` | Makes a new sequence from a range of frames of another | `entity.ExtractAnimSequence(first, last, sequence)` | fbengine E7 | Keys from first to last inclusive, moved to start at 0, and the length is last - first, not + 1 (animator.cpp `extractSeq`): looping it blends the last frame into the first. The source sequence isn't checked. Gives the new index, -1 without an Animator. |
| `AnimSeq(entity)` | The sequence playing or last played | `entity.AnimSequence()` | fbengine E7 | -1 without an Animator. |
| `AnimTime#(entity)` | The current time in frames | `entity.AnimTime()` | fbengine E7 | -1 without an Animator. |
| `AnimLength(entity)` | The current sequence's length in frames | `entity.AnimLength()` | fbengine E7 | -1 without an Animator. LoadAnimMesh's Animator starts with sequence 0's length; one AddAnimSeq made says 0 until Animate or SetAnimTime picks a sequence. |
| `Animating(entity)` | Whether it's animating or in a transition | `entity.Animating()` | fbengine E7 | False without an Animator. |

## Behaviors to match exactly

E4's done-when is "transforms, picks, and collision responses match Blitz3D's source", and E5's
is reference scenes matching Blitz3D's screenshots, so these are its test list. Each names the
source it comes from.

### Transforms and the hierarchy

1. **Rotation order and signs.** A rotation is `yawMatrix(y) * pitchMatrix(p) * rollMatrix(r)`,
   column vectors, columns i, j, k (geom.h `rotationMatrix`; geom.cpp `rotationQuat` builds the
   same as quaternions). `pitchMatrix(q)`: j = (0, cos q, sin q), k = (0, -sin q, cos q).
   `yawMatrix(q)`: i = (cos q, 0, sin q), k = (-sin q, 0, cos q). `rollMatrix(q)`:
   i = (cos q, sin q, 0), j = (-sin q, cos q, 0). Port it as matrices: geom.h's `Quat::operator*`
   multiplies in the reverse of Hamilton's order (`q.v.cross(v)`) and its pitch and roll
   quaternions use -angle/2, which cancel out only together.
2. **Reading angles back** (geom.h `quatPitch`, `quatYaw`, `matrixRoll`, `Vector::pitch`,
   `Vector::yaw`): pitch = -atan2(k.y, sqrt(k.x² + k.z²)), yaw = -atan2(k.x, k.z),
   roll = atan2(i.y, j.y), from the world or local rotation quaternion, not the scaled matrix.
   So pitch is always -90..90, and RotateEntity e, 100, 0, 0 reads back as pitch 80, yaw ±180,
   roll 180.
3. **Matrices** (entity.cpp `getLocalTform`, `getWorldTform`): local = rotation with column i
   × scale.x, j × scale.y, k × scale.z, then the position; world = parent's world × local.
   World rotation is parent's world rotation × local rotation, and world scale is parent's world
   scale × local scale per component (`getWorldRotation`, `getWorldScale`), not decompositions.
   `setLocalRotation` normalizes. `setLocalTform` (EntityParent with global, `setWorldTform`)
   decomposes into column lengths and an orthogonalized rotation (geom.h `matrixQuat`), so
   reparenting under a non-uniformly scaled, rotated parent drops shear.
4. **Space per command** (bbblitz3d.cpp, the transformation commands): MoveEntity adds local
   rotation × v (no scale); TranslateEntity adds v in the parent's space, or the world's;
   TurnEntity post-multiplies locally and pre-multiplies the world rotation globally;
   PositionEntity global applies the parent's inverse world matrix; ScaleEntity global divides
   per component by the parent's world scale; RotateEntity global applies the parent's inverse
   world rotation; PointEntity sets the world rotation; EntityX..Roll default to local.
5. **Tree order** (entity.cpp `insert`, `setParent`): top-level entities in creation order,
   children in parenting order, a reparented entity last among its new siblings, EntityParent to
   the same parent a no-op. Every walk is depth first, parent first (world.cpp `enumEnabled`,
   `enumVisible`). This order decides which eight lights shine, which collision target is
   tested where, who keeps collision entries, and the opaque draw order.
6. **Creating shows** (bbblitz3d.cpp `insert`): every created, loaded, or copied entity and its
   whole subtree is made shown and enabled and has its collisions reset.
7. **CopyEntity** (object.cpp `copy`, each class's copy constructor): deep copy of the tree;
   meshes, planes, MD2s, and BSPs share their geometry; brushes copy by value; collision type,
   radii, box, pick mode, obscurer, order, and name copy; animators copy their keys and objects
   (through `getLastCopy`) and aren't animating; cameras, lights, and terrains become plain
   Objects.
8. **FindChild** includes the entity itself, depth first. GetChild is 1-based.

### Collisions (world.cpp `World::update`, `World::collide`, `collided`, `hitTest`; collision.cpp)

9. **The update loop.** Recycle last update's entries; walk shown entities in tree order and
   file each with a nonzero type under its type; then for each shown entity in tree order:
   clear its entries and advance its animation (`beginUpdate`), run `collide` if its type isn't
   0, then set velocity = (world position - previous) / elapsed and previous = world transform
   (`endUpdate`).
10. **What's swept.** Only the source's own move, from its previous world position to its current
    one. If they're equal within 0.000001 per axis nothing is tested, and a difference that small
    is undone. Targets are tested at their previous world transform: one already processed this
    update is where it ended up, one not yet processed is where it was at the end of the last
    update. A target's own movement is never swept, so a moving door pushes nothing.
11. **Entries on both sides.** Each hit adds an entry to the source (with = target) and to the
    target (with = source), at the same point and normal. A target later in tree order clears
    its entries at its own turn, so it only keeps entries from sources after it.
12. **The ellipsoid.** With radii x ≠ y, y is scaled by x / y for the source's line and every
    target's transform (`y_tform`), making a sphere of radius x. SPHERE targets are spheres of
    their x radius (their y radius ignored). POLYGON targets use their whole transform, scale
    included. BOX targets have their transform's axes normalized (`t.m.i.normalize()` and so on),
    which removes their scale and the squash with it: the box isn't squashed while the line is.
    Port it as is.
13. **The response loop**, up to 10 hits: find the nearest hit over every enabled pair and
    target (`Collision::update` rejects t > time, so ties go to the later target); on the 10th,
    stop. Record it (point = line × time - normal × radius.x, then y × 1/squash). Make the hit's
    plane, move it out by 0.001 (COLLISION_EPSILON), and if the line crosses it at t > 0, move
    the start there and shrink the remaining length td and horizontal length td_xz by (1 - t).
    STOP: end at the start. Else the destination becomes its nearest point on the plane; with a
    previous plane, if that point is behind it, the destination is the nearest point on the two
    planes' crease line, and parallel planes are "squished" (hits = 10). With two previous
    planes, a point behind either ends the move. If the new move points against the original
    direction (dot ≤ 0), end. SLIDE: scale the move down to td if longer; SLIDE_XZ: scale it
    down so its horizontal length is td_xz; either ends if the move is under 0.000001. Loop.
    After: with 1 to 9 hits the entity moves to the end (y unsquashed) by its world position;
    with 10 it goes back to where it started this update.
14. **Triangles are one-sided** (collision.cpp `triangleCollide`): only a move against the face's
    normal, (v1 - v0) × (v2 - v0), which faces the viewer for Blitz's clockwise front faces, can
    hit. A start more than 0.001 behind the hit plane is rejected (`update`), so an entity that
    starts inside a wall passes out of it. With a radius, missed faces fall back to edge
    cylinders and corner spheres (`edgeTest`).
15. **The box test's early return** (collision.cpp `boxCollide`): `if( t>time ) return false;`
    leaves the face loop and reports no hit even after an earlier face hit and updated the time,
    so that hit is lost for that target (the time stays lowered). Rare; keep it.
16. **Mesh colliders** (meshcollider.cpp): an AABB tree, leaves of at most 16 triangles, split at
    the median of triangle centers along the longest axis, rebuilt when the geometry changes;
    triangles in world space through the whole transform. Terrains test their full-resolution
    grid (terrainrep.cpp `collide`), planes the infinite plane from above (planemodel.cpp).
17. **No rotation response.** A collision only ever sets the world position; the children
    follow it.

### Picks (world.cpp `traceRay`, `checkLOS`; bbblitz3d.cpp `doPick` and the pick commands)

18. Every shown entity with a pick mode, nearest time wins, a tie goes to the later; the point is
    line × time - normal × radius. A miss resets only the time to 1 and the entity to 0.
19. EntityPick's line is the world forward axis with scale × range, from the entity, which isn't
    excluded. CameraPick's and CameraProject's coordinates are viewport-relative, ProjectedZ is
    the near range, and CameraPick's perspective line starts at the camera.
20. EntityVisible: every shown entity but the two, with a pick mode and the obscurer flag, against
    the center-to-center line, radius 0.

### Rendering (world.cpp `render`, model.cpp, brush.cpp, meshmodel.cpp, gxscene.cpp)

21. **Cameras**: shown cameras with a projection, by EntityOrder, highest first. Each sets its
    viewport, clears color (alpha 1) and depth per its clear mode, sets its projection and fog,
    draws each shown mirror's reflected pass with culling flipped, then the normal pass.
22. **Pass order** in each: models with a positive order, highest first, depth off, each one's
    transparent queue right after it; models of order 0 in tree order, depth on, opaque parts
    drawn as they come; then all their transparent parts sorted far to near by the distance from
    the camera to each entity's origin, depth tested but not written; then negative orders,
    -1 before -2, depth off, each with its transparent parts. Sorting is per entity, never per
    surface or triangle.
23. **What's transparent** (brush.cpp `getBlend`): a surface's combined brush is opaque unless its
    blend resolves to something else: an explicit multiply or add; alpha when one of the layers'
    textures has the alpha flag without the mask flag; auto when it has exactly one layer and it's
    such a texture; and for alpha or auto, FX 32 or alpha below 1. Explicit alpha with none of
    those is opaque. A skinned mesh's non-opaque surfaces all go to the transparent queue
    (meshmodel.cpp `render`). One entity's transparent surfaces draw in reverse surface order
    (model.cpp `renderQueue` pops from the back).
24. **Combining brushes** (brush.cpp `Brush(a, b)`): color × color, alpha × alpha, shininess +
    shininess, the entity's blend if not 0, FX OR'd, the entity's texture per layer where it has
    one.
25. **Lighting**, DX7's fixed function, per vertex, Gouraud-interpolated (gxscene.cpp,
    gxlight.cpp): with M the material color (the combined brush color, or the vertex color with
    FX 2, as D3D's diffuse and ambient sources), the lit color is clamp(A × M + Σ over the first
    eight shown lights of att × spot × C × M × max(0, N·L), 0, 1), where A is AmbientLight (white
    with FX 1, BSPAmbientLight on a BSP), C a light's color (unclamped), att 1 for directional and
    range / d for point and spot (D3D attenuation1 = 1/range alone, no cutoff), and spot the cone
    term with whole angles theta and phi and falloff 1. Normals are normalized (NORMALIZENORMALS).
    Alpha is the material's alpha. Specular, with shininess s > 0: Σ att × spot × white ×
    clamp(s, 0, 1) × max(0, N·H)^(128 × s), Blinn-Phong with D3D's default local viewer, added
    after texturing. Emissive is 0, except BSP's FX_EMISSIVE faces, whose vertex colors are
    emissive. FX 1 turns every light off and ambient white, so the result is exactly M. FX 4 is
    flat shading (D3D's: the first vertex's color). FX_CONDLIGHT (BSP) leaves directional lights
    off. Directional lights shine along their forward axis.
26. **Texture layers** (gxscene.cpp `setRenderState`, `setTexState`): each layer with a texture
    and a blend other than 0 becomes the next stage, so blend-0 layers close up. Color: arg1 the
    texture, arg2 the stage below; op ALPHA lerps by the texture's alpha, MULTIPLY modulates, ADD
    adds, DOT3 is D3D's dot product, MULTIPLY2 modulates × 2. Alpha: modulate when the texture
    has the alpha flag, else the stage below's. Stage 0's "below" is the lit color. UVs from the
    layer's set, through its matrix (texture.cpp `getMatrix`), wrap unless clamped. Sphere maps
    use the camera-space normal × (0.5, -0.5, 0.5) + 0.5 (`sphere_mat`); cube maps per
    SetCubeMode.
27. **Then** specular is added, fog mixes toward the fog color by (end - depth) / (end - start)
    clamped (unless FX 8), the alpha test runs if layer 0's texture is masked (alpha > int(128 ×
    alpha), or > 0 with FX 32), and the framebuffer blend is the resolved blend: alpha (src α,
    1 - src α), multiply (dest × src), add (src × src α + dest).
28. **Culling**: clockwise front faces, so counterclockwise culled, flipped in mirror passes;
    FX 16 off.
29. **Sampling**: always bilinear with linear mipmap blending (gxscene.cpp's constructor sets
    MIN, MAG, and MIP to linear and nothing changes them); mipmaps only with flag 8, box-filtered
    2 × 2 (ddutil.cpp `buildMipMaps`); every texture stretched to power-of-two sizes on load.
30. **Texture alpha on load** (ddutil.cpp): mask: alpha 0 for exactly black, else 255; alpha flag
    on an image without alpha: alpha = (r + g + b) / 3, and RGB whitened without flag 1; no
    alpha flag: 255. The texture filter list ORs flags in by substring, default "" → 1 + 8.
31. **Alpha and fading** (model.cpp): alpha 0 skips drawing; RenderWorld's tween lerps alpha;
    AutoFade multiplies alpha by 1 - (d - near) / (far - near) and skips past far.
32. **Packing without clamps** (gxscene.cpp `setAmbient`, `setFogColor`, `clear`): ambient, fog,
    and clear colors are packed as int(v × 255) shifted, so values outside 0..255 bleed into the
    next byte. fbengine clamps; a game that relies on the bleed is unlikely.

### Sprites, planes, mirrors, terrain

33. **Sprites** (sprite.cpp `render`): the quad's corners (±1 - handle.x, ±1 - handle.y, 0) go
    through the view mode's matrix × roll(rotation) × scale(sprite x, sprite y, 1); mode 1 takes
    the camera's matrix whole, mode 3 sets k to the camera's and orthogonalizes, mode 4 is the
    camera's yaw × the entity's matrix. CreateSprite starts full-bright; LoadSprite's blend is
    replace (masked), alpha, or add, from its flags.
34. **Planes** (planemodel.cpp): rebuilt each frame from the far frustum projected to y = 0,
    UVs the local x and z, not drawn from below, collides from above.
35. **Mirrors** (world.cpp `render`): the camera reflected through each mirror's local XZ plane,
    the whole scene drawn before the normal pass, no clipping.
36. **Terrain** (terrainrep.cpp): byte heights that wrap at `size`, UVs (x, size - z), normals up
    unless shaded, bilinear TerrainY, LoadTerrain's max(r, g, b) with the image flipped in z.

### Animation (animator.cpp, animation.cpp, md2model.cpp)

37. Keys at integer frames; position and scale lerp, rotation slerps the short way, held outside
    the keys; a channel with no keys leaves that part of the transform alone.
38. Animate restarts the time every call; mode 0 with a nonzero speed poses frame 0 and stops;
    transitions lerp and slerp from the current pose to the start over `transition` units of
    elapsed with time frozen; loop wraps by fmod, ping-pong reflects and flips the speed's sign,
    once clamps and stops.
39. SetAnimTime stops animating and wraps the time; ExtractAnimSeq's length is last - first.
40. MD2: loops blend last - 1 into first (`render_b==anim_last` goes to first); transitions step
    1/transition per UpdateWorld call regardless of elapsed.
41. Hidden entities don't animate (UpdateWorld only advances shown ones).

## Design questions

1. **Space and units.** fbengine in std.math's right-handed, -Z-forward, radian space, with the
   migrator converting at every call (z negated, pitch negated, triangle corners swapped,
   boxes and matrices by the rules in Conventions), or a left-handed fbengine. *Recommendation:*
   right-handed. std.math, glTF, Vulkan through std's `Mat4.Perspective`, and Jolt are all
   right-handed, engine.md already says the importers and converter account for Blitz's
   handedness, and the conversions are mechanical at the API boundary. The terrain grid then runs
   along -Z, so grid coordinates pass through unchanged.
2. **Euler angles in radians or degrees.** *Recommendation:* radians, as std says angles are
   everywhere, with the migrator writing `Deg(...)` and `ToDegrees(...)` and folding literals
   (`Deg(1)` stays readable). engine.md's `cube.Turn(0, 1, 0)` then means one radian; its example
   should become `cube.Turn(0, Deg(1), 0)`.
3. **Mesh and Geometry.** Blitz's mesh entity owns surfaces shared by CopyEntity, which ScaleMesh
   and friends change for every copy. *Recommendation:* split them: a `Mesh` entity draws a
   `Geometry` (surfaces), `Copy()` shares it, and geometry commands live on `Geometry`, so the
   sharing is visible, it matches glTF's mesh and node, and E8 can cache `Geometry` as an asset.
   The alternative is Blitz-style Methods on Mesh (`mesh.ScaleMesh(v)`) that silently change
   copies.
4. **Material: shared or by value.** Blitz paints by copy-on-write value: changing a brush after
   painting changes nothing painted, and GetSurfaceBrush and GetEntityBrush give copies.
   *Recommendation:* `Material` a shared, refcounted Class, as glTF materials and E6 want, with no
   Free (FreeBrush dropped, ClearWorld's brushes a no-op), and the migrator writing `.Clone()` at
   each paint and get, which keeps Blitz's semantics exactly. If E6's materials own GPU
   resources, give them Free and keep the Clones.
5. **Color space of the Blitz look.** DX7 did lighting, texture blending, fog, and framebuffer
   blending on display-space (gamma-encoded) values. fbengine's `Color` is linear.
   *Recommendation:* the migrator writes `Color.Srgb(r / 255, g / 255, b / 255)`, and
   `Shading.VERTEX` turns every color back into display space (std's ToSrgb, exact over all
   reals) for DX7's math. For blending to match too, the camera renders into a UNORM target with
   textures sampled as UNORM, not sRGB: a renderer setting, say
   `Renderer.SetColorSpace(ColorSpace.DISPLAY)`, which the migrator turns on. Blending in linear
   light makes alpha and additive effects visibly different.
6. **Results as Structs, Blitz's globals in a helper.** *Recommendation:* fbengine returns `Pick?`,
   `Collision[]`, and `Vec3`; `fbengine.blitz` keeps PickedX, TFormedX, and ProjectedX as globals
   with Blitz's staleness (a miss keeps the last point, normal, surface, and triangle), so
   migrated code reads as Blitz's and modern code never sees globals.
7. **Port or reimplement collisions, picks, and terrain.** *Recommendation:* port world.cpp's
   `collide`, `traceRay`, and `checkLOS`, collision.cpp, meshcollider.cpp, and terrainrep.cpp's
   collision to fbengine_native's C++ as they are (zlib allows it, with the notice), quirks
   included: target entries cleared by order, stale target positions, the box test's early
   return, the 10-hit panic, squashed normals. Games are tuned to these. E10's Jolt is the modern
   path, and there's no "fixed" Blitz collision mode to maintain.
8. **Old formats at run time.** engine.md converts .b3d, .x, and .3ds with fbengine_tools.
   *Recommendation:* E8's importer runs the converter on import, keeping the asset's path, so
   `Mesh.Load("res://models/guard.b3d")` loads the cooked glTF and the migrator leaves paths
   alone; LoadAnimSeq's files the same. MD2 and BSP load at run time in E4, as engine.md's E4
   lists. LoaderMatrix becomes a converter option per extension.
9. **Eight lights.** *Recommendation:* `Shading.VERTEX` uses only the first eight shown lights in
   tree order, as DX7 did, since games placed lights around the limit; PBR shading has no limit.
10. **Viewports, units, and resizing.** Blitz's viewport is the 2D viewport when the camera was
    made, in back-buffer pixels, and never follows a resize. *Recommendation:* a camera with no
    CameraViewport covers the whole window and follows resizes, which equals Blitz's in a window
    that isn't resized, and viewport, CameraPick, and CameraProject coordinates are in E3's 2D
    units.
11. **Power-of-two textures.** Blitz stretches every texture to power-of-two sizes and reports
    them; CreateTexture(100, 100) is 128 × 128, and a game drawing into it relies on 100/128 of
    the UVs. *Recommendation:* `TextureOptions.PowerOfTwo` (False by default), which the
    migrator sets on every texture it creates or loads.
12. **Texture caching.** Blitz shares pixels between loads of one path and flag set, but each
    load has its own blend, UV set, and transform. E8's cache gives the same asset for one path.
    *Recommendation:* a `Texture` is a view (blend, UV set, UV transform, cube mode) over a cached
    image, and `Texture.Load` gives a new view over the shared image, so both hold. Drawing into
    one load's Target changes the other, as in Blitz.
13. **Typing migrated handles.** Blitz's are Ints and EntityColor on a pivot is a debug-only
    error. *Recommendation:* typed classes; the migrator infers each variable's class from what
    made it and writes checked casts, `(e As Model)`, where it can't, which panic as Blitz's debug
    builds stop. The alternative, every Method on Entity with run-time checks, loses fbengine's
    compile-time checks for everyone.
14. **CopyEntity of cameras, lights, and terrains.** Blitz makes pivots of them (no clone of
    their own). *Recommendation:* copy them properly, with the migrator warning at such a call.
15. **Animate's restart.** *Recommendation:* keep Animate exactly (restarts on each call), since
    the migrator can't tell intent; E7's modern clips get a `Play` that doesn't restart a playing
    clip.
16. **Index width.** *Recommendation:* 32-bit triangle indices; Blitz's 16-bit limit isn't
    behavior anyone needs.
17. **Collision types.** *Recommendation:* keep Int32 types 0 to 999 (games already name them with
    Consts) rather than a `CollisionGroup` type, so Collisions and EntityType map one to one.
18. **Which quirks to keep.** *Recommendation:* keep every quirk a game can observe and might be
    tuned to (FitMesh's uniform tie, stale picks, collision order and panic, LoadSprite's additive
    default, ProjectedZ, viewport-relative picks, cone normals, terrain wrap), and fix crashes and
    undefined behavior (color packing overflow, ModifyTerrain over 1, SetAnimTime's off-by-one,
    LoadBrush's double delete) by clamping or panicking.
19. **Where `fbengine.blitz` lives.** *Recommendation:* in fbengine, built in E4 (the space and
    color converters, which E4's tests against Blitz's source need too) and finished in E13 (the
    last-result globals and EntityClass's words, which only the migrator needs).
20. **Sound on entities.** EmitSound and CreateListener tie fbengine_audio to the scene, but
    engine.md keeps each package's DLL to itself. *Recommendation:* fbengine_audio's FB code
    imports fbengine and, at each RenderWorld (as Blitz updates them), copies the listener's and
    emitters' entity positions and velocities into its own channels; no C++ links across.
21. **World's Functions.** *Recommendation:* `UpdateWorld()`, `CaptureWorld()`, and
    `RenderWorld()` stay Blitz-like Functions, as engine.md's frame loop has them; the rest of
    the world's settings are `World`'s Static Methods, as `Window`'s are.

## Counts

234 commands mapped, none skipped. By where they land (an E4 row whose drawing or lighting is
E5's counts as E4):

| Where | Commands |
|---|---|
| fbengine E4 | 200 |
| fbengine E5 | 10 |
| fbengine E6 | 1 |
| fbengine E7 | 14 |
| fbengine_audio E9 | 2 |
| fbengine_tools E13 | 1 |
| dropped | 6 |
| std, std (new), language | 0 |
| Total | 234 |

Of the 200 in E4, 26 are scene objects or settings whose effect on the image is E5's (marked "lit
by E5", "drawn by E5", "the look, E5", or "flags' effects, E5"), and 2 have parts in E7
("animation E7"). The dropped six: HWMultiTex, WBuffer, and Dither (nothing to set on modern
GPUs), HWTexUnits and GfxDriverCaps3D (constants, 8 and 110), and VertexW (always 1).

Skipped as internal: none. bbblitz3d.cpp registers no `_` or `__` names; its helpers
(`insert`, `erase`, `findChild`, `collapseMesh`, `loadEntity`, `terrainHeight`,
`terrainVector`, `doPick`, the debug checks) aren't commands.
