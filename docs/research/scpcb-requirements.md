# SCP: Containment Breach v1.3.11: what fbengine must provide

Source read: SCRATCH/scpcb (the game's code at v1.3.11, `VersionNumber` at Main.bb:45), Blitz3D's
source in SCRATCH/blitz3d, and its command lists in SCRATCH/b3d-commands. SCP:CB is CC BY-SA 3.0
(README.md); this file describes behavior and numbers for a from-scratch remake and copies no code.
"Main.bb:2817" means file and line in SCRATCH/scpcb; Blitz3D's source is cited as
`blitz3d/<path>:<line>`.

**The game build** is Main.bb and what it includes, 24 files (Main.bb:19-32, 108, 304, 348, 1546,
1819, 1823, 2427, 2431, 2688, 2756, 7823, 8919; MapSystem.bb:829, 7903). The other 8 of the 32 .bb
files are tools, not the game: Converter.bb, RMesh_Model_Viewer.bb, LightMapPNG.bb (FreeImage),
CPU_Details.bb (cpuid), MusicPlayer.bb (includes a BlitzAL.bb that isn't in the repo),
`DrawPortals – Kopio.bb`, and Map Creator/mapcreator_new.bb and window3d.bb (the map editor that
writes .cbmap2). DrawPortals.bb is included (MapSystem.bb:829) but never called: its only use is
commented out (MapSystem.bb:2137-2141), and it needs FastExt's `FE_RENDER`, which nothing loads.

**A modified Blitz3D.** The game calls five functions that are in no command list, no .decls, and
no .bb (found by SCRATCH/scpcb_unknown.py): `ErrorLog()` (Main.bb:11689), `TextureLodBias`
(Main.bb:7210, 8296), and `TextureBumpEnvMat`, `TextureBumpEnvOffset`, `TextureBumpEnvScale`
(MapSystem.bb:36-41, 305-310), with a `TextureBlend` mode 6 (bump environment map) that stock
Blitz3D doesn't have (blitz3d/gxruntime/gxscene.h:45-50 stops at 5). SCP:CB 1.3.11 was built
with a patched runtime (probably the "MAV-less" Blitz3D in the file list's mavless_bin.zip).
Besides `ErrorLog`, which writes the crash log (`CatchErrors`, Main.bb:11688), these serve only the
bump-mapping option and the texture-detail option.

---

## 1. Commands used

Counted by SCRATCH/scpcb_count.py (run with `python -I`): every name in b3d-commands/*.txt (a
leading `%`, `#`, or `$` stripped; names starting with `_` are internal and skipped), matched
case-insensitively as whole words in the .bb files, with strings, `;` comments, field accesses
(`n\Sound`), type names (`.Items`), and `Field` lines left out, and `End` counted only alone (not
`End If`, `End Function`, ...). "Game" is the 24-file build; "All" is all 32 files. Raw counts and
each command's per-file counts are in SCRATCH/scpcb-counts.json. `End` (4) is the updater's and the
launcher's quit (Update.bb, Menu.bb:213); `Stop` is never called.

The calls to know about are the wrapped ones: the game loads through StrictLoads.bb's wrappers
(`LoadImage_Strict`, `LoadSound_Strict`, `PlaySound_Strict`, `LoadMesh_Strict`,
`LoadAnimMesh_Strict`, `LoadTexture_Strict`, `LoadBrush_Strict`, `LoadFont_Strict`), so the
commands under them (`LoadMesh` 3, `LoadAnimMesh` 1, `PlaySound` 6) look rarer than they are, and
it draws text through AAText.bb's `AAText` (on `Text` and sprites). It never calls `Animate`,
`Load3DSound`, `EmitSound`, `CreateListener`, `LoopSound`, `CreateTerrain`, `LoadBSP`, `LoadMD2`,
`CreateMirror`, `SetCubeMode`, any `Joy*`, `CreateTimer`, or `Graphics` (only `Graphics3D`, through
`Graphics3DExt`, Main.bb:11246-11253).

536 commands in the lists; the game calls 297 of them, its tools 24 more, and 215 are called nowhere.

| # | Command | List | Game | All 32 files |
|---:|---|---|---:|---:|
| 1 | `EntityX` | bbblitz3d | 1000 | 1012 |
| 2 | `EntityZ` | bbblitz3d | 999 | 1011 |
| 3 | `PositionEntity` | bbblitz3d | 808 | 825 |
| 4 | `EntityY` | bbblitz3d | 753 | 765 |
| 5 | `Rand` | bbmath | 629 | 629 |
| 6 | `RotateEntity` | bbblitz3d | 531 | 547 |
| 7 | `EntityYaw` | bbblitz3d | 526 | 535 |
| 8 | `Rnd` | bbmath | 384 | 384 |
| 9 | `EntityParent` | bbblitz3d | 366 | 381 |
| 10 | `DebugLog` | bbruntime | 276 | 309 |
| 11 | `CreatePivot` | bbblitz3d | 275 | 282 |
| 12 | `ReadFloat` | bbstream | 269 | 330 |
| 13 | `EntityDistance` | bbblitz3d | 263 | 267 |
| 14 | `Color` | bbgraphics | 260 | 344 |
| 15 | `HideEntity` | bbblitz3d | 240 | 261 |
| 16 | `Chr` | bbstring | 230 | 327 |
| 17 | `FreeEntity` | bbblitz3d | 209 | 231 |
| 18 | `Sin` | bbmath | 181 | 183 |
| 19 | `PointEntity` | bbblitz3d | 181 | 182 |
| 20 | `ShowEntity` | bbblitz3d | 165 | 174 |
| 21 | `MoveEntity` | bbblitz3d | 151 | 162 |
| 22 | `TurnEntity` | bbblitz3d | 137 | 140 |
| 23 | `ResetEntity` | bbblitz3d | 134 | 134 |
| 24 | `ScaleEntity` | bbblitz3d | 133 | 146 |
| 25 | `CopyEntity` | bbblitz3d | 133 | 142 |
| 26 | `ChannelPlaying` | bbaudio | 133 | 133 |
| 27 | `Len` | bbstring | 131 | 189 |
| 28 | `ReadByte` | bbstream | 129 | 185 |
| 29 | `Rect` | bbgraphics | 121 | 174 |
| 30 | `EntityPitch` | bbblitz3d | 114 | 121 |
| 31 | `EntityTexture` | bbblitz3d | 114 | 118 |
| 32 | `ReadInt` | bbstream | 110 | 161 |
| 33 | `WriteFloat` | bbstream | 109 | 160 |
| 34 | `EntityType` | bbblitz3d | 102 | 104 |
| 35 | `DrawImage` | bbgraphics | 101 | 112 |
| 36 | `Floor` | bbmath | 100 | 113 |
| 37 | `Lower` | bbstring | 97 | 132 |
| 38 | `ScaleSprite` | bbblitz3d | 94 | 94 |
| 39 | `Instr` | bbstring | 92 | 144 |
| 40 | `AnimTime` | bbblitz3d | 92 | 92 |
| 41 | `EntityAlpha` | bbblitz3d | 91 | 99 |
| 42 | `Cos` | bbmath | 83 | 83 |
| 43 | `Right` | bbstring | 75 | 99 |
| 44 | `EntityFX` | bbblitz3d | 71 | 76 |
| 45 | `EntityVisible` | bbblitz3d | 71 | 71 |
| 46 | `Left` | bbstring | 65 | 107 |
| 47 | `StopChannel` | bbaudio | 60 | 60 |
| 48 | `WriteByte` | bbstream | 59 | 91 |
| 49 | `SetBuffer` | bbgraphics | 59 | 88 |
| 50 | `PeekInt` | bbbank | 56 | 56 |
| 51 | `ReadString` | bbstream | 51 | 80 |
| 52 | `FreeTexture` | bbblitz3d | 51 | 67 |
| 53 | `EntityPickMode` | bbblitz3d | 51 | 55 |
| 54 | `BankSize` | bbbank | 50 | 50 |
| 55 | `BackBuffer` | bbgraphics | 47 | 64 |
| 56 | `EntityBlend` | bbblitz3d | 47 | 47 |
| 57 | `EntityRoll` | bbblitz3d | 46 | 51 |
| 58 | `ImageWidth` | bbgraphics | 46 | 50 |
| 59 | `ImageHeight` | bbgraphics | 44 | 48 |
| 60 | `Text` | bbgraphics | 42 | 103 |
| 61 | `WriteInt` | bbstream | 42 | 84 |
| 62 | `DeltaYaw` | bbblitz3d | 42 | 42 |
| 63 | `Cls` | bbgraphics | 41 | 69 |
| 64 | `TextureBuffer` | bbblitz3d | 41 | 62 |
| 65 | `TFormedX` | bbblitz3d | 41 | 54 |
| 66 | `EntityInView` | bbblitz3d | 41 | 44 |
| 67 | `SetAnimTime` | bbblitz3d | 40 | 40 |
| 68 | `TFormedZ` | bbblitz3d | 39 | 52 |
| 69 | `TranslateEntity` | bbblitz3d | 39 | 41 |
| 70 | `MeshWidth` | bbblitz3d | 36 | 39 |
| 71 | `EntityRadius` | bbblitz3d | 36 | 36 |
| 72 | `Trim` | bbstring | 35 | 82 |
| 73 | `RuntimeError` | bbruntime | 34 | 43 |
| 74 | `FreeBank` | bbbank | 34 | 34 |
| 75 | `CreateSprite` | bbblitz3d | 33 | 33 |
| 76 | `WriteLine` | bbstream | 31 | 115 |
| 77 | `Mid` | bbstring | 31 | 53 |
| 78 | `CreateBank` | bbbank | 31 | 31 |
| 79 | `CloseFile` | bbfilesystem | 28 | 71 |
| 80 | `AddVertex` | bbblitz3d | 28 | 56 |
| 81 | `FileType` | bbfilesystem | 28 | 37 |
| 82 | `MeshHeight` | bbblitz3d | 28 | 30 |
| 83 | `ClsColor` | bbgraphics | 27 | 43 |
| 84 | `EntityColor` | bbblitz3d | 27 | 28 |
| 85 | `MilliSecs` | bbruntime | 25 | 29 |
| 86 | `ImageBuffer` | bbgraphics | 24 | 27 |
| 87 | `TFormPoint` | bbblitz3d | 24 | 25 |
| 88 | `ChannelVolume` | bbaudio | 24 | 24 |
| 89 | `EntityPick` | bbblitz3d | 23 | 24 |
| 90 | `TFormedY` | bbblitz3d | 22 | 35 |
| 91 | `MeshDepth` | bbblitz3d | 22 | 24 |
| 92 | `ReadLine` | bbstream | 21 | 49 |
| 93 | `KeyDown` | bbinput | 21 | 37 |
| 94 | `CameraRange` | bbblitz3d | 21 | 29 |
| 95 | `FreeImage` | bbgraphics | 21 | 22 |
| 96 | `PokeInt` | bbbank | 21 | 21 |
| 97 | `AddTriangle` | bbblitz3d | 20 | 38 |
| 98 | `SetFont` | bbgraphics | 19 | 20 |
| 99 | `SpriteViewMode` | bbblitz3d | 19 | 19 |
| 100 | `Replace` | bbstring | 18 | 44 |
| 101 | `MaskImage` | bbgraphics | 18 | 25 |
| 102 | `CameraFogColor` | bbblitz3d | 18 | 18 |
| 103 | `ResumeChannel` | bbaudio | 18 | 18 |
| 104 | `CameraClsColor` | bbblitz3d | 17 | 19 |
| 105 | `MouseYSpeed` | bbinput | 17 | 18 |
| 106 | `EntityOrder` | bbblitz3d | 17 | 17 |
| 107 | `Flip` | bbgraphics | 16 | 34 |
| 108 | `BrushTexture` | bbblitz3d | 16 | 28 |
| 109 | `CreateTexture` | bbblitz3d | 16 | 24 |
| 110 | `FreeBrush` | bbblitz3d | 16 | 23 |
| 111 | `GetSurface` | bbblitz3d | 16 | 23 |
| 112 | `FlushKeys` | bbinput | 16 | 19 |
| 113 | `MouseXSpeed` | bbinput | 16 | 17 |
| 114 | `WriteString` | bbstream | 15 | 41 |
| 115 | `Eof` | bbstream | 14 | 38 |
| 116 | `KeyHit` | bbinput | 14 | 33 |
| 117 | `CreateMesh` | bbblitz3d | 14 | 27 |
| 118 | `CameraFogRange` | bbblitz3d | 14 | 14 |
| 119 | `CameraProjMode` | bbblitz3d | 14 | 14 |
| 120 | `Ceil` | bbmath | 14 | 14 |
| 121 | `ReadFile` | bbfilesystem | 13 | 35 |
| 122 | `TFormVector` | bbblitz3d | 13 | 25 |
| 123 | `TextureBlend` | bbblitz3d | 13 | 21 |
| 124 | `TextureName` | bbblitz3d | 13 | 21 |
| 125 | `PickedY` | bbblitz3d | 13 | 16 |
| 126 | `PickedEntity` | bbblitz3d | 13 | 15 |
| 127 | `CloseTCPStream` | bbsockets | 13 | 13 |
| 128 | `CopyRect` | bbgraphics | 13 | 13 |
| 129 | `CreateSurface` | bbblitz3d | 12 | 23 |
| 130 | `StringWidth` | bbgraphics | 12 | 22 |
| 131 | `RenderWorld` | bbblitz3d | 12 | 16 |
| 132 | `MouseZSpeed` | bbinput | 12 | 12 |
| 133 | `GetChild` | bbblitz3d | 11 | 20 |
| 134 | `MouseHit` | bbinput | 11 | 17 |
| 135 | `PickedX` | bbblitz3d | 11 | 14 |
| 136 | `PickedZ` | bbblitz3d | 11 | 14 |
| 137 | `Delay` | bbruntime | 10 | 17 |
| 138 | `GetBrushTexture` | bbblitz3d | 10 | 13 |
| 139 | `CameraZoom` | bbblitz3d | 10 | 12 |
| 140 | `GetSurfaceBrush` | bbblitz3d | 10 | 12 |
| 141 | `AmbientLight` | bbblitz3d | 10 | 10 |
| 142 | `ColorRed` | bbgraphics | 10 | 10 |
| 143 | `PauseChannel` | bbaudio | 10 | 10 |
| 144 | `SeedRnd` | bbmath | 10 | 10 |
| 145 | `MouseX` | bbinput | 9 | 39 |
| 146 | `MouseY` | bbinput | 9 | 38 |
| 147 | `MoveMouse` | bbinput | 9 | 13 |
| 148 | `CreateCamera` | bbblitz3d | 9 | 12 |
| 149 | `CameraViewport` | bbblitz3d | 9 | 10 |
| 150 | `CameraFogMode` | bbblitz3d | 9 | 9 |
| 151 | `MeshCullBox` | bbblitz3d | 9 | 9 |
| 152 | `PeekByte` | bbbank | 9 | 9 |
| 153 | `Upper` | bbstring | 8 | 25 |
| 154 | `CountSurfaces` | bbblitz3d | 8 | 20 |
| 155 | `ReadPixelFast` | bbgraphics | 8 | 14 |
| 156 | `PaintSurface` | bbblitz3d | 8 | 11 |
| 157 | `CreateImage` | bbgraphics | 8 | 8 |
| 158 | `FlushMouse` | bbinput | 8 | 8 |
| 159 | `UpdateWorld` | bbblitz3d | 8 | 8 |
| 160 | `VectorYaw` | bbblitz3d | 8 | 8 |
| 161 | `Line` | bbgraphics | 7 | 19 |
| 162 | `LoadTexture` | bbblitz3d | 7 | 19 |
| 163 | `WriteFile` | bbfilesystem | 7 | 18 |
| 164 | `Sqr` | bbmath | 7 | 14 |
| 165 | `OpenFile` | bbfilesystem | 7 | 13 |
| 166 | `CurrentDir` | bbfilesystem | 7 | 11 |
| 167 | `UnlockBuffer` | bbgraphics | 7 | 11 |
| 168 | `VertexTexCoords` | bbblitz3d | 7 | 11 |
| 169 | `LockBuffer` | bbgraphics | 7 | 10 |
| 170 | `FileSize` | bbfilesystem | 7 | 9 |
| 171 | `HidePointer` | bbgraphics | 7 | 9 |
| 172 | `Collisions` | bbblitz3d | 7 | 8 |
| 173 | `CurrentDate` | bbstring | 7 | 7 |
| 174 | `FreeSound` | bbaudio | 7 | 7 |
| 175 | `LoadSound` | bbaudio | 7 | 7 |
| 176 | `AddMesh` | bbblitz3d | 6 | 20 |
| 177 | `VertexColor` | bbblitz3d | 6 | 9 |
| 178 | `CloseDir` | bbfilesystem | 6 | 8 |
| 179 | `NextFile` | bbfilesystem | 6 | 8 |
| 180 | `ReadDir` | bbfilesystem | 6 | 8 |
| 181 | `PlaySound` | bbaudio | 6 | 7 |
| 182 | `CurrentTime` | bbstring | 6 | 6 |
| 183 | `GfxModeHeight` | bbgraphics | 6 | 6 |
| 184 | `GfxModeWidth` | bbgraphics | 6 | 6 |
| 185 | `ProjectedY` | bbblitz3d | 6 | 6 |
| 186 | `ReadAvail` | bbstream | 6 | 6 |
| 187 | `ResizeBank` | bbbank | 6 | 6 |
| 188 | `TextureHeight` | bbblitz3d | 5 | 11 |
| 189 | `DeleteFile` | bbfilesystem | 5 | 10 |
| 190 | `CreateDir` | bbfilesystem | 5 | 7 |
| 191 | `GetKey` | bbinput | 5 | 6 |
| 192 | `Asc` | bbstring | 5 | 5 |
| 193 | `FontHeight` | bbgraphics | 5 | 5 |
| 194 | `FreeFont` | bbgraphics | 5 | 5 |
| 195 | `Oval` | bbgraphics | 5 | 5 |
| 196 | `ReadBytes` | bbbank | 5 | 5 |
| 197 | `WireFrame` | bbblitz3d | 5 | 5 |
| 198 | `WriteBytes` | bbbank | 5 | 5 |
| 199 | `StringHeight` | bbgraphics | 4 | 11 |
| 200 | `End` | bbruntime | 4 | 10 |
| 201 | `CopyMesh` | bbblitz3d | 4 | 9 |
| 202 | `TextureWidth` | bbblitz3d | 4 | 9 |
| 203 | `FlipMesh` | bbblitz3d | 4 | 8 |
| 204 | `MouseDown` | bbinput | 4 | 7 |
| 205 | `ScaleTexture` | bbblitz3d | 4 | 7 |
| 206 | `LoadFont` | bbgraphics | 4 | 5 |
| 207 | `ShowPointer` | bbgraphics | 4 | 5 |
| 208 | `SystemProperty` | bbruntime | 4 | 5 |
| 209 | `CameraClsMode` | bbblitz3d | 4 | 4 |
| 210 | `CameraProject` | bbblitz3d | 4 | 4 |
| 211 | `ChannelPan` | bbaudio | 4 | 4 |
| 212 | `LoadAnimTexture` | bbblitz3d | 4 | 4 |
| 213 | `OpenTCPStream` | bbsockets | 4 | 4 |
| 214 | `ProjectedX` | bbblitz3d | 4 | 4 |
| 215 | `ResizeImage` | bbgraphics | 4 | 4 |
| 216 | `WritePixelFast` | bbgraphics | 4 | 4 |
| 217 | `AppTitle` | bbruntime | 3 | 10 |
| 218 | `LoadMesh` | bbblitz3d | 3 | 10 |
| 219 | `VertexX` | bbblitz3d | 3 | 10 |
| 220 | `VertexZ` | bbblitz3d | 3 | 10 |
| 221 | `GetParent` | bbblitz3d | 3 | 9 |
| 222 | `PositionMesh` | bbblitz3d | 3 | 9 |
| 223 | `UpdateNormals` | bbblitz3d | 3 | 5 |
| 224 | `AlignToVector` | bbblitz3d | 3 | 3 |
| 225 | `AntiAlias` | bbblitz3d | 3 | 3 |
| 226 | `ATan2` | bbmath | 3 | 3 |
| 227 | `CollisionY` | bbblitz3d | 3 | 3 |
| 228 | `CopyBank` | bbbank | 3 | 3 |
| 229 | `CountCollisions` | bbblitz3d | 3 | 3 |
| 230 | `CreateLight` | bbblitz3d | 3 | 3 |
| 231 | `DeltaPitch` | bbblitz3d | 3 | 3 |
| 232 | `EntityAutoFade` | bbblitz3d | 3 | 3 |
| 233 | `FontWidth` | bbgraphics | 3 | 3 |
| 234 | `LightColor` | bbblitz3d | 3 | 3 |
| 235 | `LightRange` | bbblitz3d | 3 | 3 |
| 236 | `LinePick` | bbblitz3d | 3 | 3 |
| 237 | `NameEntity` | bbblitz3d | 3 | 3 |
| 238 | `PokeByte` | bbbank | 3 | 3 |
| 239 | `LoadImage` | bbgraphics | 2 | 21 |
| 240 | `CountVertices` | bbblitz3d | 2 | 10 |
| 241 | `MidHandle` | bbgraphics | 2 | 9 |
| 242 | `RotateImage` | bbgraphics | 2 | 6 |
| 243 | `ExecFile` | bbruntime | 2 | 4 |
| 244 | `HandleImage` | bbgraphics | 2 | 4 |
| 245 | `VertexCoords` | bbblitz3d | 2 | 4 |
| 246 | `ScaleMesh` | bbblitz3d | 2 | 3 |
| 247 | `ActiveTextures` | bbblitz3d | 2 | 2 |
| 248 | `ChannelPitch` | bbaudio | 2 | 2 |
| 249 | `ColorBlue` | bbgraphics | 2 | 2 |
| 250 | `ColorGreen` | bbgraphics | 2 | 2 |
| 251 | `DeleteDir` | bbfilesystem | 2 | 2 |
| 252 | `DrawImageRect` | bbgraphics | 2 | 2 |
| 253 | `GetColor` | bbgraphics | 2 | 2 |
| 254 | `PickedNX` | bbblitz3d | 2 | 2 |
| 255 | `PickedNY` | bbblitz3d | 2 | 2 |
| 256 | `PickedNZ` | bbblitz3d | 2 | 2 |
| 257 | `PositionTexture` | bbblitz3d | 2 | 2 |
| 258 | `ScaleImage` | bbgraphics | 2 | 2 |
| 259 | `GraphicsWidth` | bbgraphics | 1 | 19 |
| 260 | `GraphicsHeight` | bbgraphics | 1 | 13 |
| 261 | `CountChildren` | bbblitz3d | 1 | 9 |
| 262 | `EntityName` | bbblitz3d | 1 | 6 |
| 263 | `VertexY` | bbblitz3d | 1 | 6 |
| 264 | `FilePos` | bbfilesystem | 1 | 5 |
| 265 | `Graphics3D` | bbgraphics | 1 | 5 |
| 266 | `RotateMesh` | bbblitz3d | 1 | 5 |
| 267 | `CreateBrush` | bbblitz3d | 1 | 4 |
| 268 | `LoadAnimMesh` | bbblitz3d | 1 | 3 |
| 269 | `TextureCoords` | bbblitz3d | 1 | 3 |
| 270 | `CameraPick` | bbblitz3d | 1 | 2 |
| 271 | `AvailVidMem` | bbgraphics | 1 | 1 |
| 272 | `ClearSurface` | bbblitz3d | 1 | 1 |
| 273 | `ClearWorld` | bbblitz3d | 1 | 1 |
| 274 | `CountGfxDrivers` | bbgraphics | 1 | 1 |
| 275 | `CountGfxModes3D` | bbgraphics | 1 | 1 |
| 276 | `CreateCube` | bbblitz3d | 1 | 1 |
| 277 | `CreatePlane` | bbblitz3d | 1 | 1 |
| 278 | `EntityBox` | bbblitz3d | 1 | 1 |
| 279 | `EntityCollided` | bbblitz3d | 1 | 1 |
| 280 | `EntityShininess` | bbblitz3d | 1 | 1 |
| 281 | `FindChild` | bbblitz3d | 1 | 1 |
| 282 | `GetEntityType` | bbblitz3d | 1 | 1 |
| 283 | `GfxDriverName` | bbgraphics | 1 | 1 |
| 284 | `GraphicsBuffer` | bbgraphics | 1 | 1 |
| 285 | `Hex` | bbstring | 1 | 1 |
| 286 | `ImageRectOverlap` | bbgraphics | 1 | 1 |
| 287 | `LightConeAngles` | bbblitz3d | 1 | 1 |
| 288 | `LoadAnimImage` | bbgraphics | 1 | 1 |
| 289 | `LoadBrush` | bbblitz3d | 1 | 1 |
| 290 | `LoadSprite` | bbblitz3d | 1 | 1 |
| 291 | `PeekShort` | bbbank | 1 | 1 |
| 292 | `ReadShort` | bbstream | 1 | 1 |
| 293 | `RndSeed` | bbmath | 1 | 1 |
| 294 | `SeekFile` | bbfilesystem | 1 | 1 |
| 295 | `TotalVidMem` | bbgraphics | 1 | 1 |
| 296 | `TrisRendered` | bbblitz3d | 1 | 1 |
| 297 | `Windowed3D` | bbgraphics | 1 | 1 |

Called only by the tools (Map Creator, RMesh_Model_Viewer, LightMapPNG, Converter, MusicPlayer, CPU_Details, `DrawPortals – Kopio`), never by the game:

| Command | List | All 32 files |
|---|---|---:|
| `TriangleVertex` | bbblitz3d | 15 |
| `CountTriangles` | bbblitz3d | 7 |
| `LSet` | bbstring | 6 |
| `CopyFile` | bbfilesystem | 5 |
| `CopyImage` | bbgraphics | 4 |
| `Print` | bbgraphics | 4 |
| `Input` | bbgraphics | 3 |
| `VertexU` | bbblitz3d | 3 |
| `VertexV` | bbblitz3d | 3 |
| `ChangeDir` | bbfilesystem | 2 |
| `EntityClass` | bbblitz3d | 2 |
| `Graphics` | bbgraphics | 2 |
| `VertexBlue` | bbblitz3d | 2 |
| `VertexGreen` | bbblitz3d | 2 |
| `VertexRed` | bbblitz3d | 2 |
| `WaitKey` | bbinput | 2 |
| `CaptureWorld` | bbblitz3d | 1 |
| `CreateSphere` | bbblitz3d | 1 |
| `GetMatElement` | bbblitz3d | 1 |
| `String` | bbstring | 1 |
| `VertexAlpha` | bbblitz3d | 1 |
| `VertexNX` | bbblitz3d | 1 |
| `VertexNY` | bbblitz3d | 1 |
| `VertexNZ` | bbblitz3d | 1 |

Never called by the game (the tools-only ones above included), by list:

- **basic** (1): `RuntimeStats`
- **bbaudio** (7): `Load3DSound`, `LoopSound`, `PlayCDTrack`, `PlayMusic`, `SoundPan`, `SoundPitch`, `SoundVolume`
- **bbbank** (4): `CallDLL`, `PeekFloat`, `PokeFloat`, `PokeShort`
- **bbblitz3d** (90): `AddAnimSeq`, `Animate`, `AnimateMD2`, `Animating`, `AnimLength`, `AnimSeq`, `BrushAlpha`, `BrushBlend`, `BrushColor`, `BrushFX`, `BrushShininess`, `BSPAmbientLight`, `BSPLighting`, `CaptureWorld`, `ClearCollisions`, `ClearTextureFilters`, `CollisionEntity`, `CollisionNX`, `CollisionNY`, `CollisionNZ`, `CollisionSurface`, `CollisionTime`, `CollisionTriangle`, `CollisionX`, `CollisionZ`, `CountTriangles`, `CreateCone`, `CreateCylinder`, `CreateListener`, `CreateMirror`, `CreateSphere`, `CreateTerrain`, `Dither`, `EmitSound`, `EntityClass`, `ExtractAnimSeq`, `FindSurface`, `FitMesh`, `GetEntityBrush`, `GetMatElement`, `GfxDriverCaps3D`, `HandleSprite`, `HWMultiTex`, `HWTexUnits`, `LightMesh`, `LoadAnimSeq`, `LoadBSP`, `LoaderMatrix`, `LoadMD2`, `LoadTerrain`, `MD2Animating`, `MD2AnimLength`, `MD2AnimTime`, `MeshesIntersect`, `ModifyTerrain`, `PaintEntity`, `PaintMesh`, `PickedSurface`, `PickedTime`, `PickedTriangle`, `ProjectedZ`, `RotateSprite`, `RotateTexture`, `SetAnimKey`, `SetCubeFace`, `SetCubeMode`, `Stats3D`, `TerrainDetail`, `TerrainHeight`, `TerrainShading`, `TerrainSize`, `TerrainX`, `TerrainY`, `TerrainZ`, `TextureFilter`, `TFormNormal`, `TriangleVertex`, `VectorPitch`, `VertexAlpha`, `VertexBlue`, `VertexGreen`, `VertexNormal`, `VertexNX`, `VertexNY`, `VertexNZ`, `VertexRed`, `VertexU`, `VertexV`, `VertexW`, `WBuffer`
- **bbfilesystem** (2): `ChangeDir`, `CopyFile`
- **bbgraphics** (55): `AutoMidHandle`, `BufferDirty`, `CloseMovie`, `CopyImage`, `CopyPixel`, `CopyPixelFast`, `CountGfxModes`, `DrawBlock`, `DrawBlockRect`, `DrawMovie`, `EndGraphics`, `FrontBuffer`, `GammaBlue`, `GammaGreen`, `GammaRed`, `GfxDriver3D`, `GfxMode3D`, `GfxMode3DExists`, `GfxModeDepth`, `GfxModeExists`, `GrabImage`, `Graphics`, `GraphicsDepth`, `GraphicsLost`, `ImageRectCollide`, `ImagesCollide`, `ImagesOverlap`, `ImageXHandle`, `ImageYHandle`, `Input`, `LoadBuffer`, `Locate`, `MovieHeight`, `MoviePlaying`, `MovieWidth`, `OpenMovie`, `Origin`, `Plot`, `Print`, `ReadPixel`, `RectsOverlap`, `SaveBuffer`, `SaveImage`, `ScanLine`, `SetGamma`, `SetGfxDriver`, `TFormFilter`, `TFormImage`, `TileBlock`, `TileImage`, `UpdateGamma`, `Viewport`, `VWait`, `Write`, `WritePixel`
- **bbinput** (28): `DirectInputEnabled`, `EnableDirectInput`, `FlushJoy`, `GetJoy`, `GetMouse`, `JoyDown`, `JoyHat`, `JoyHit`, `JoyPitch`, `JoyRoll`, `JoyType`, `JoyU`, `JoyUDir`, `JoyV`, `JoyVDir`, `JoyWait`, `JoyX`, `JoyXDir`, `JoyY`, `JoyYaw`, `JoyYDir`, `JoyZ`, `JoyZDir`, `MouseWait`, `MouseZ`, `WaitJoy`, `WaitKey`, `WaitMouse`
- **bbmath** (7): `ACos`, `ASin`, `ATan`, `Exp`, `Log`, `Log10`, `Tan`
- **bbruntime** (7): `CommandLine`, `CreateTimer`, `FreeTimer`, `GetEnv`, `SetEnv`, `Stop`, `WaitTimer`
- **bbsockets** (18): `AcceptTCPStream`, `CloseTCPServer`, `CloseUDPStream`, `CountHostIPs`, `CreateTCPServer`, `CreateUDPStream`, `DottedIP`, `HostIP`, `RecvUDPMsg`, `SendUDPMsg`, `TCPStreamIP`, `TCPStreamPort`, `TCPTimeouts`, `UDPMsgIP`, `UDPMsgPort`, `UDPStreamIP`, `UDPStreamPort`, `UDPTimeouts`
- **bbstream** (2): `CopyStream`, `WriteShort`
- **bbstring** (4): `Bin`, `LSet`, `RSet`, `String`
- **multiplay** (14): `CreateNetPlayer`, `DeleteNetPlayer`, `HostNetGame`, `JoinNetGame`, `NetMsgData`, `NetMsgFrom`, `NetMsgTo`, `NetMsgType`, `NetPlayerLocal`, `NetPlayerName`, `RecvNetMsg`, `SendNetMsg`, `StartNetGame`, `StopNetGame`

---

## 2. Userlibs and DLLs

Main.bb:10-17 refuses to start without fmod.dll and zlibwapi.dll. What each .decls declares, what
the game calls from it (counts are the game build's; from scpcb-counts.json), what for, and what
the remake uses instead.

### fmod.decls (fmod.dll, FMOD 3.x)

| Function | Calls | Where | For |
|---|---:|---|---|
| `FSOUND_Init` | 1 | FMod.bb:16 | `FSOUND_Init(44100, 64, 0)` at startup (FMod.bb:1-16: 44100 Hz, 64 channels) |
| `FSOUND_Stream_Open` | 1 | StrictLoads.bb:163 | opens an .ogg as a stream, mode 2 = looping (FMod.bb:5), 0 = once |
| `FSOUND_Stream_Play` | 1 | StrictLoads.bb:173 | plays it on a free channel |
| `FSOUND_SetVolume` | 2 | StrictLoads.bb:183, 220 | volume x 255 |
| `FSOUND_SetPaused` | 3 | StrictLoads.bb:184, 221, 237 | pause and resume (menus, inventory, console) |
| `FSOUND_SetPan` | 1 | StrictLoads.bb:274 | pan -1..1 mapped to 0..255 |
| `FSOUND_IsPlaying` | 1 | StrictLoads.bb:253 | end of an intro video's audio, announcements |
| `FSOUND_StopSound`, `FSOUND_Stream_Stop`, `FSOUND_Stream_Close` | 1, 2, 1 | StrictLoads.bb:201-203, Menu.bb | stop and free |

Used for all music (`Music$()` names, Main.bb:1560-1586, played by `UpdateMusic`,
Main.bb:8992-9026), SCP-096's and SCP-079's voice lines (NPCs.bb:1257-1563, UpdateEvents.bb:5927,
5958, 5990), the intercom announcements (`PlayAnnouncement`, Main.bb:11764-11773), the ending
narration (UpdateEvents.bb:9319), and the startup videos' audio (Main.bb:11883, 11905): 19 calls of
`StreamSound_Strict`. Every other sound is Blitz's own `LoadSound`/`PlaySound` (which is FMOD 3 too,
inside Blitz3D's gxaudio). **Remake: fbengine_audio (E9)**, streamed sounds with volume, pan,
pause, and loop; the same Ogg Vorbis files.

### BlitzMovie.decls (BlitzMovie.dll)

| Function | Calls | Where | For |
|---|---:|---|---|
| `BlitzMovie_Open`, `_GetWidth`, `_GetHeight`, `_Close` | 2, 2, 2, 4 | Main.bb:11876-11879, 11898-11901 | read the movie's size |
| `BlitzMovie_OpenDecodeToImage` | 2 | Main.bb:11881, 11903 | decode into a Blitz image each frame |
| `BlitzMovie_Play`, `BlitzMovie_Stop` | 2, 2 | Main.bb:11882, 11889 | |

Only `PlayStartupVideos()` (Main.bb:11851-11922): GFX/menu/startup_Undertow.avi and startup_TSS.avi,
silent AVIs with their sound in a separate .ogg stream, drawn by copying the image into a 2048x2048
texture on a full-screen quad (`ProjectImage`, Main.bb:11924-11947), skipped by any key or by the
audio ending, and turned off by the `play startup video` option (Main.bb:11853). **Remake: video
playback is missing from docs/engine.md.** Either add a small decoder to fbengine (MPEG-1 through
pl_mpeg, or Theora, re-encoding the two AVIs once) as an E3 or E8 step, or convert both clips to
image sequences, or drop them (they're two logos).

### DevilParticleSystem.decls (`.lib " "`)

A `.lib " "` decls is no DLL: it only declares, for the IDE, 31 Functions that DevilParticleSystem.bb
defines itself (DevilParticleSystem.bb:48-298, bytecode77's system, CPU sprite particles; the .bb's
update is `UpdateParticles_Devil`, as Particles.bb has its own `UpdateParticles`). Outside that file
the game calls 20 of them (the template setters, `InitParticles`, `FreeParticles`, and
`SetEmitter`). Used for three templates made in `LoadEntities` (Main.bb:8304-8365): 0 electric sparks (light cones with flickering lights,
MapSystem.bb:8002; the console's spawnparticles, Main.bb:1405), 1 vent smoke, 2 decontamination gas with a sub-template, and emitters set by
events and `CreateDevilEmitter` (Particles.bb:198-288), updated at most once per 70 Hz tick
(Main.bb:2995-3000). **Remake: game code in FutureBlitz** on fbengine's sprites and meshes (E4); a
port of the .bb, not an engine feature. E6's GPU particles aren't needed for this.

### FreeImage.decls (FreeImage.dll)

`FI_Load`, `FI_Save`, `FI_Unload`: only LightMapPNG.bb, a tool that converts lightmaps; the game
calls nothing from it. Its RtlMoveMemory entries aren't called. **Remake: drop** (std.image reads
and writes PNG, JPEG, BMP).

### kernel32.decls, user32.decls, gdi32.decls

Hundreds of Win32 declarations; the game calls six:

| Function | Calls | Where | For | Remake |
|---|---:|---|---|---|
| `api_SetWindowLong`, `api_SetWindowPos` | 2, 2 | Main.bb:120-121, 163-164 | borderless windowed: WS_POPUP at the desktop's size | E1 step 1's `WindowMode.BORDERLESS` |
| `api_GetClientRect`, `api_GetDesktopWindow` | 1, 1 | fullscreen_window_fix.bb:70 | the desktop's size | E1's `Displays()` desktop mode |
| `api_GetFocus` | 1 | Main.bb:7120 | pause (`Delay 1000` a frame, Main.bb:7125) while the window isn't focused | E1 step 2's `Window.Focused()` |
| `GlobalMemoryStatus` | 2 | Main.bb:4852, 11700 | debug HUD and error log | `std.os`'s `System.TotalMemory()`, or drop |
| `api_GetModuleFileName` | 1 | Update.bb:876 | the updater's own .exe name | drop |

gdi32 is called by nothing. Map Creator/window3d.bb also calls `api_FindWindow`, `api_GetActiveWindow`,
and `api_ShowWindow` (the editor's own window).

### zlibwapi.decls (zlibwapi.dll) and Blitz_File_ZipApi.bb

23 `ZlibWapi_*` functions (`UnzOpen`, `UnzLocateFile` 8, `UnzReadCurrentFile`, `ZipOpen`, `Crc32`,
`Compress2`, ...), all called only from Blitz_File_ZipApi.bb, whose only caller is the updater:
`CheckForUpdates` (Update.bb:567, called from the launcher at Menu.bb:1165) downloads a .zip over
`OpenTCPStream` HTTP and unpacks it (Update.bb:785-839), when `check for updates` is on
(Update.bb:12, 580). **Remake: drop** the updater; releases update through fb's own channels. If a
zip is ever needed, `std.compress.zip`'s `ZipReader` (docs/stdlib.md:155).

### cpuid.decls (cpuid.dll)

`CPUid`, `CPUbrand`, ...: only CPU_Details.bb, a tool. **Drop.**

### Also in the repo

dplayx.dll (DirectPlay, for Blitz's `multiplay` commands, none called) and the 32-bit Blitz runtime
DLLs: drop. The remake is 64-bit only.

---

## 3. Asset formats

What SCRATCH/scpcb-files.txt lists (2116 files): 895 .ogg, 523 .jpg, 277 .png, 113 .rmesh, 112 .b3d,
56 .x, 29 .bmp, 7 .ttf, 2 .avi, 2 .cbmap2, 1 .dds, and files with made-up extensions (below).

### .rmesh, the room format (MapSystem.bb:336-753, `LoadRMesh`)

All 96 room templates in Data/rooms.ini use .rmesh (`mesh path`, MapSystem.bb:1660; none uses .b3d),
and `LoadEntities` loads the 7 maintenance-tunnel pieces mt1, mt2, mt2c, mt3, mt4, mt_elevator, and
mt_generator (Main.bb:8280-8293). The repo ships the converter that makes them ("B3D - RoomMesh
Conversion.exe"). All numbers are little-endian, as Blitz's streams write them: an int is 4 bytes, a
float 4, a byte 1, and a **string is a 4-byte length then that many bytes, no terminator**
(blitz3d/bbruntime/bbstream.cpp:58-70).

1. **Header**: a string, `"RoomMesh"` or `"RoomMesh.HasTriggerBox"` (anything else is an error,
   MapSystem.bb:375-382). Texture names below are relative to the .rmesh's folder (MapSystem.bb:384).
2. **Drawn meshes**: an int count (MapSystem.bb:394), then for each:
   - **Two texture slots**, slot 0 the lightmap and slot 1 the diffuse texture (MapSystem.bb:412-441).
     Each is a byte flag, and if the flag isn't 0, a string file name. The flag decides how it loads:
     1 loads with Blitz flags 1 (color) and `TextureBlend` 5 (modulate 2x); 2 loads with flags 1 and
     the default blend (2, multiply); 3 loads with flags 3 (color and alpha) and makes the whole
     surface an alpha surface. A name containing `_lm` (lower-cased) gets `TextureBlend` 3 (add)
     instead, whatever its flag. Textures are cached by file name without its path
     (`GetTextureFromCache`, `AddTextureToCache`, MapSystem.bb:274-318), the same cache
     Data/materials.ini fills. Slot j's texture reads UV set 1-j (`TextureCoords`, MapSystem.bb:438):
     **the lightmap reads UV set 1, the diffuse UV set 0.**
   - **The brush** (MapSystem.bb:443-480): an alpha surface gets only its diffuse texture, blend 2,
     in layer 0 (or a blank white 4x4 texture). An opaque surface with both textures gets **layer 0
     `AmbientLightRoomTex`** (a 2x2 texture cleared to black, blend 5, Main.bb:7860-7866), an
     optional layer 1 bump map (blend 6, from materials.ini's `bump`, when bump mapping is on), then
     the lightmap and then the diffuse texture in the next two layers. With one texture, each sits in
     its own slot's layer, with blank white in the empty one. So a room pixel is
     `(2 x ambientTex x vertexColor + lightmap) x diffuse`: black ambient leaves the baked lightmap,
     and night vision raises `AmbientLightRoomTex` to 15 grey (`AmbientLightRooms(15)`,
     Main.bb:4521, MapSystem.bb:8169-8190) to lift every room at once.
   - A first, empty surface is made and then a second one gets the data (MapSystem.bb:405, 476): each
     drawn mesh has an extra empty surface. The brush is painted only when a texture was found.
   - **Vertices**: an int count, then per vertex 3 floats x, y, z; 2 floats u, v for UV set 0; 2
     floats for UV set 1; 3 bytes r, g, b (alpha 1.0) (MapSystem.bb:482-500). No normals: the rooms
     are fullbright, so normals are never needed.
   - **Triangles**: an int count, then 3 ints per triangle, indexes into this mesh's vertices
     (MapSystem.bb:502-506).
   - **Where it goes** (MapSystem.bb:508-523): an alpha surface joins the `Alpha` mesh and **doesn't
     collide**. An opaque one joins the `Opaque` mesh, and a copy of it, made double-sided by adding
     a flipped copy, becomes an invisible (alpha 0) collision child, `HIT_MAP`, polygon-pickable.
3. **Invisible collision meshes** (MapSystem.bb:528-547): an int count; each is one surface: an int
   vertex count, 3 floats per vertex, an int triangle count, 3 ints per triangle, added in both
   windings. They're one `hiddenMesh`, alpha 0, `HIT_MAP`: collision the player never sees (ramps
   over stairs, blockers).
4. **Trigger boxes**, only after the `HasTriggerBox` header (MapSystem.bb:549-572): an int count;
   each is an int surface count, surfaces as in step 3 (both windings), then a string name. Events
   test the player against them by name (`CheckTriggers`, Main.bb:11645-11678); they draw only with
   the debug HUD (MapSystem.bb:5658-5672).
5. **Point entities** (MapSystem.bb:574-715): an int count; each starts with a string class name.
   Positions are multiplied by `RoomScale` as they're read, except `playerstart`'s and `model`'s:
   - `screen`: 3 floats position, a string image (in GFX/screens). A clickable spot that shows the
     image full-screen (`CreateScreen`, `UpdateScreens`, MapSystem.bb:6235-6272). Skipped at 0,0,0.
   - `waypoint`: 3 floats. A pathfinding node.
   - `light`: 3 floats; a float range, divided by 2000; a string color "r g b"; a float intensity,
     made `Min(intensity x 0.8, 1)`, each color channel multiplied by it. A point light (type 2).
     At 0,0,0 its fields are read and dropped.
   - `spotlight`: as `light`, then a string angles "pitch yaw roll" and two ints, the inner and
     outer cone angles. **The .rmesh loader makes these point lights too** (type 2,
     MapSystem.bb:640), though `FillRoom` only sets cone angles for type 3 (MapSystem.bb:5509-5513):
     spotlights in rooms are point lights. (The unused .b3d loader makes them type 3, MapSystem.bb:165.)
   - `soundemitter`: 3 floats; an int, the index of a `[room ambience]` sound in rooms.ini (1 to 10:
     rumble, low drone, pulsing, ventilation, drip, alarm, 895, fuel pump, fan, servers); a float
     range, in world units (not scaled). At most 8 per room (`MaxRoomEmitters`, MapSystem.bb:1602); extras are read and dropped.
   - `playerstart`: 3 floats, a string angles. Ignored (it positions an undeclared `cam`).
   - `model`: a string file in GFX/Map/Props; then 3 floats position, 3 rotation (pitch, yaw,
     roll), 3 scale, in the room's own units. Loaded once per file and copied (`CreatePropObj`,
     MapSystem.bb:7007-7019), parented to the room's drawn mesh, `HIT_MAP`, polygon-pickable. **An
     empty file name reads only the 3 position floats**, which would put the next 6 out of step: the
     shipped files must never have one. A class name the loader doesn't know reads nothing and
     would break the rest the same way.
6. **The room object** (MapSystem.bb:717-751): the alpha mesh gets a flipped copy (double-sided)
   and joins the opaque one; `EntityFX 3` (fullbright and vertex colors) on all of it. The root
   pivot's children are, in order: 1 an empty pivot, **2 the drawn mesh**, 3 the invisible
   collision mesh, 4 and 5 empty pivots, 6 the pivot of collision copies. The game hides a room's
   drawing by `EntityAlpha(GetChild(room, 2), 0)` (MapSystem.bb:5682-5701), so the order matters to
   any port that keeps it.

Each placed room is `CopyEntity` of its template, scaled by **RoomScale = 8/2048** (MapSystem.bb:1748,
2040-2042), `HIT_MAP`, polygon-pickable, at a grid cell x 8 (MapSystem.bb:7474-7563). A room is 2048
rmesh units, 8 world units, wide. The template's lights, screens, waypoints, trigger boxes, and sound
emitters are made for each room at the room's position plus their offsets, before the room is
turned, and parented to it, so turning the room turns them (MapSystem.bb:5506-5550, then
`TurnEntity` at 7491-7563). Each light also gets two sprites, a small one (0.13, light1.jpg, additive)
and a flickering halo (0.6, lightsprite.jpg, additive, `EntityOrder -1`), and a pick pivot
(MapSystem.bb:5721-5788).

**Remake**: the .rmesh format is **not in docs/engine.md or docs/assets.md** (assets.md's converter
handles .b3d, .x, and .3ds). Two choices: a loader in the game's own FutureBlitz code over
`std.io`'s `BinaryReader` and fbengine's mesh-building API (E4), which keeps the files as they
ship; or an .rmesh importer in fbengine_tools' converter (E8 or E13) that writes glTF (two UV
sets, vertex colors, lightmap and diffuse as separate textures, the collision and trigger meshes as
extra nodes) and the point entities as a sidecar. The second fits E8's cooker; either way the
renderer needs E5's layered texture blending to draw it as Blitz did.

### .cbmap2 and .cbmap, custom maps (Save.bb:2120-2380, `LoadMap`)

Written by the Map Creator, listed from "Map Creator\Maps" (Save.bb:2060-2117), chosen in the menu.

- **.cbmap2** (Save.bb:2140-2321): a line (`ReadLine`, to CR/LF) author; a line description; a byte
  zone transition 0 and a byte transition 1 (rows where the zones change; generated maps use 13 and
  7, MapSystem.bb:7026-7027); ints room count, forest piece count, and maintenance-tunnel piece count.
  Each room: byte x, byte y, string name, byte angle (x 90 degrees), string event name, float event
  probability (0 with a name: always; above 0: `Rnd(0,1) <= p`). The x is mirrored (`MapWidth - x`)
  and angles other than 90 and 270 get 180 added (Save.bb:2163-2175). Each forest piece (SCP-860-1's
  10x10 grid): byte x, byte y, string name ("scp-860-1 endroom", "path", "corner", "t-shaped
  path", "4-way path", "door"), byte angle, remapped (Save.bb:2228-2256). Each tunnel piece (a 19x19
  grid in room2tunnel): byte x, byte y, string name ("maintenance tunnel endroom", "corridor",
  "corner", "t-shaped room", "4-way room", "elevator", "generator room"), byte angle, remapped
  (Save.bb:2283-2316).
- **.cbmap**, the old format (Save.bb:2322-2380): rooms only, records as above, read until the end
  of the file; transitions fixed at 13 and 7.

Doors between rooms are then made as for a generated map (Save.bb:2385 on). **Remake**: game code,
over `std.io`'s `BinaryReader`; nothing for the engine.

### Meshes: .b3d and .x

Loaded with Blitz's `LoadMesh` (static, one merged mesh) or `LoadAnimMesh` (a hierarchy, with bones
and keyframes), always through StrictLoads.bb:301-314:

- `LoadAnimMesh_Strict` 38 calls, all .b3d: every NPC (classd.b3d, guard.b3d, MTF2.b3d,
  scp-049.b3d, zombie1.b3d, clerk.b3d, 106_2.b3d, 035tentacle.b3d, scp-066.b3d, scp-939.b3d,
  205_*.b3d, ...), apache.b3d and its rotors, 079.b3d, and animated items (clipboard.b3d,
  wallet.b3d, `Anim` set, Items.bb:41-43, 275, 303). These are **skinned meshes with bones**:
  `FindChild` finds bones by name for `ManipulateNPCBones` (NPCs.bb:7168-7235, Data/NPCBones.ini),
  which turns a head bone toward the camera **after** animation (Main.bb:3009).
- `LoadMesh_Strict` 84 calls (67 .b3d, 17 .x): doors (door01.x, doorframe.x, heavydoor1/2.x,
  ContDoorLeft/Right.x, elevatordoor.b3d), buttons, levers, cameras, monitors, props, 173_2.b3d
  (SCP-173 is static, moved whole), the forest's terrain details, pocket dimension pieces.
- 133 item templates (Items.bb:115-333) load 97 .x and 36 .b3d (scale 0.0004 to 0.003, textured
  with flags 9 by default, Items.bb:31).
- `CreatePropObj` loads props by whatever name an .rmesh gives (MapSystem.bb:7017, plain `LoadMesh`):
  GFX/Map/Props holds .x, .b3d, and bathroomsinks.smf, which Blitz3D can't load by that extension
  (and toilet02.smf in GFX/map), so either an .rmesh never names them or that prop silently fails.
- No `.3ds`, `.md2`, or `.bsp`.

Animation is only ever `SetAnimTime` with a fractional frame (40 calls, through `AnimateNPC` and
`Animate2`, Main.bb:9338-9417): the game advances a frame number by `speed x FPSfactor` itself,
clamped or looped between two frames of the one sequence, and Blitz interpolates the keys. No
`Animate`, `AnimSeq`, `ExtractAnimSeq`, or blending between sequences. `UpdateWorld` is called each
frame (`UpdateWorld(0)` while paused, Main.bb:3005-3010) for collisions. Frame numbers are tuned to
each file's keyframes (e.g. 173's box, SCP-106's `SetAnimTime n\obj, 110`, NPCs.bb:990), so **a
converted model must keep Blitz's frame numbering**, not seconds.

LoadWorld (MapSystem.bb:54-255) reads properties from .b3d node names ("classname=..." lines,
`KeyValue`, MapSystem.bb:791-811) for rooms made in a .b3d; no room uses it.

**Remake**: E7 (skeletons from .b3d, `SetAnimTime`) and E13's converter (.b3d and .x to glTF,
left-handed to right-handed). The converter must keep node and bone names, the frame-to-time
mapping, per-surface brushes with their texture file names, and vertex colors.

### Textures

`LoadTexture_Strict(file, flags = 1)` (StrictLoads.bb:317-322) adds 256 (VRAM) when `enable vram`
is on. **Blitz3D's default texture filter adds color and mipmapping to every texture loaded from
a file** (`TextureFilter "", 1+8`, blitz3d/bbruntime/bbblitz3d.cpp:1937-1938;
blitz3d/blitz3d/texture.cpp:21-27), so every loaded texture is mipmapped. Flags in
calls (counted over the game's files): 1 (color) 67, 1+2 (alpha) 31, 9 (items, Items.bb:31), 3 (an
.rmesh alpha surface), 1+16+32 (clamped U and V, skyboxes, `%110001`, Skybox.bb:11). Made with
`CreateTexture`: 1+256 (512x512 security-camera targets, Main.bb:7878-7879; 2048x2048 resize and
gamma targets, Main.bb:11482-11483), 256 (the 2048x2048 blur target, Dreamfilter.bb:35), 1+2 and
1+2+256 (the 1024x1024 blink and flash overlays, Main.bb:7936, 7949), 257 (the ambient room
texture), 256|16|32 (the video quad). `LoadAnimTexture` 3 (frame strips: AgentIJ.AIJ,
Main.bb:3583, and room2sl's monitor sheets, MapSystem.bb:5241-5242).
Blend modes used: 1 alpha, 2 multiply, 3 add, 5 multiply 2x, 6 bump (modified runtime);
`ScaleTexture`, `PositionTexture`, and `TextureCoords` (UV set) are used; `TextureName` is read back
to find a surface's step sound and to swap monitor screens (Main.bb:9236-9291, 8166-8197).

Formats: .jpg, .png, .bmp (lightmaps `*_lm1.bmp`), one .dds (GFX/map/forest/detail/tree1.dds).
**Several images have made-up extensions**, as easter eggs and to hide documents: `.pt` (hg.pt,
173h.pt, creditsscreen.pt, endingscreen.pt), `.dc` (decalpd6.dc), `.it` and `.s` (bn.it, sn.it,
dh.s: document images, Items.bb), `.sc` (GFX/screens/*.sc), `.pd` (kneelmortal.pd), and `.AIJ`
(AgentIJ.AIJ, a texture strip, Main.bb). `.cwm` files are sounds (SFX/SCP/990/cwm1.cwm, Menu.bb).
The loader must **detect the format from the bytes**, not the extension.

**Remake**: E5 (texture flags, the blend modes, eight layers, clamping), E8 (loading, caching by
path, `res://` paths), stb_image (assets.md) for JPEG, PNG, BMP; DDS needs a reader (or a one-time
conversion). Paths are Windows-style (`"GFX\map\door01.x"`), case-insensitive: E8's `res://`
normalization has to accept backslashes and any case.

### Sounds

All .ogg (895 files). `LoadSound_Strict` (StrictLoads.bb:123-135) only records the name when `sfx
release` is on (the default, options.ini) and loads on first play; `AutoReleaseSounds`
(StrictLoads.bb:34-56) frees a sound 5 s after its last channel stops; `PlaySound_Strict`
(StrictLoads.bb:58-121) keeps up to 32 channels per sound and sets each new channel's volume to
`SFXVolume`. `LoadAllSounds` (LoadAllSounds.bb) registers 79 sounds up front; the rest load when
events need them, `LoadTempSound` keeps a ring of 10 temporary ones (Main.bb:8969-8977). **All 3D
sound is faked in 2D** (section 4, Sound); `Load3DSound`, `EmitSound`, and `CreateListener` are never
called (the listener is commented out at Main.bb:7883). Music and long voice lines stream through
FMOD (section 2). User tracks: any file in SFX/Radio/UserTracks that `LoadSound` accepts plays
on the radio item (Main.bb:8199-8221, 6143-6170), loaded whole, not streamed.

**Remake**: E9 (Ogg Vorbis through miniaudio, which assets.md lists): sounds, channels with volume,
pan, and pitch in Hz (`ChannelPitch tempchn, Rand(20000,30000)` for drips, Main.bb:4321), pause and
resume, and streams.

### Fonts

Five TrueType files through `AALoadFont` (Main.bb:230-238), sized to the screen: Courier New 19 and
bold 58, DS-Digital 22 and 60, Journal 58, each x `GraphicHeight / 1024`; and the console's
"Blitz" (Blitz3D's built-in font) at 20 x that. The GFX/font folders exist because Blitz3D only
loads a font whose file name matches its internal name (Main.bb:226-229). `AAText.bb` draws text
either with Blitz's `Text` or, with `antialiased text` on, from its own atlas: it renders each glyph
with GDI, reads it back with `ReadPixelFast`, writes it into an alpha texture with `WritePixelFast`,
and draws each character as a quad under its own camera (AAText.bb:27-64, 219-336). **Remake**: E3
(TrueType text) or E11 (SDF text). Metrics must match GDI's closely, as menus lay text out by
`StringWidth` and `FontHeight` (`RowText`, `LimitText`, Menu.bb:1776-1978).

### Data files

INI throughout: Data/rooms.ini (748 lines: 96 rooms plus `[room ambience]`), events.ini (map creator
descriptions), NPCs.ini (speeds and scales), NPCBones.ini, materials.ini (bump maps and step sounds),
SCP-294.ini (999 lines: drinks), achievementstrings.ini, 1499chunks.ini, Loadingscreens/
loadingscreens.ini, and options.ini, which the game also writes (`PutINIValue`, `SaveOptionsINI`,
Main.bb:11024-11180). The game's own reader (Main.bb:10847-11022) caches each file's lines, takes a
`[section]` by name, and reads `key = value`, trimmed. Credits.txt is read as lines (Main.bb:3881).
**Remake**: `std.data`'s `Ini.Read` and `Ini.Write` (docs/stdlib.md:123, 1147-1153); the files have
`;` comments and keys with spaces (`mesh path`, `shape = 2C`), which it reads.

Save games: Saves/<name>/save.txt, a binary Blitz stream (strings, ints, floats, bytes) of the player,
the map, every NPC, door, item, and event (`SaveGame`, Save.bb:2-469; `LoadGame` 470;
`LoadGameQuick` 1268). **Remake**: the game's own format through `std.io`'s `BinaryWriter`; reading
original saves is optional.

---

## 4. Game systems

### Main loop and timing (Main.bb:2809-3360)

- Each frame: `Cls`; elapsed time from `MilliSecs` (wrapped positive, `MilliSecs2`, Main.bb:2705-2709);
  **`FPSfactor = Max(Min(elapsed_seconds x 70, 5.0), 0.2)`** (Main.bb:2814-2817); `FPSfactor2` keeps
  it unpaused (for the message timer, Main.bb:3295). `FPSfactor = 0` while the pause menu, the
  inventory, a sub-inventory, the console, a keypad, a screen, or SCP-294 is open (Main.bb:2820): the
  world freezes but still draws.
- Frame limit: `framelimit` in options.ini (0 = off) delays to `1000 / limit` ms (Main.bb:2822-2828);
  `vsync` = 1 by default flips with `Flip 1` (Main.bb:3355-3359).
- Order, unpaused (Main.bb:2857-3011): `UpdateMusic`, `AutoReleaseSounds`; then `UpdateStreamSounds`,
  `UpdateSecurityCams` (renders camera screens), random ambience, fog and range from the light
  volume, `AmbientLight`, `UpdateDeafPlayer`, `UpdateEmitters`, **`MouseLook`, then `MovePlayer`**,
  `UpdateDoors`, `UpdateEvents` (or the endings or SCP-1499), `UpdateScreens`,
  `TimeCheckpointMonitors`, `Update294`, `UpdateRoomLights`, `UpdateDecals`, `UpdateMTF`,
  `UpdateNPCs`, `UpdateItems`, `UpdateParticles`, `Use427`, `UpdateMonitorSaving`, the Devil
  particles at most once per tick; then **`UpdateWorld`** (collisions) and `ManipulateNPCBones`;
  `RenderWorld2`; the blur; the dark overlay (blink, sanity, death, fall); inventory, save, and
  console keys; `DrawGUI`, `DrawMenu`, `UpdateConsole`, messages, the FPS counter, quick-load
  progress, achievement pop-ups; then the borderless-scale and gamma passes, and `Flip`.
  `UpdateRooms` (which room the player is in, which rooms draw, the light volume) runs first thing in
  `UpdateEvents` (UpdateEvents.bb:15) and again after teleports.
- **Logic isn't on a fixed tick.** Everything advances by `x FPSfactor` once per rendered frame, with
  70 the design rate (all timers count "70ths of a second": `MsgTimer = 70*4`, Main.bb:3172). But
  many things happen once per frame regardless: `Rand(1500) = 1` picks a new ambient sound
  (Main.bb:2888), `Rand(700) = 1` a 173 scrape (NPCs.bb:740), the dust particle `Rand(35) = 1`
  (Main.bb:4477), and most event and NPC rolls, so they happen twice as often at 140 fps as at 70.
  Collisions sweep once per frame, so a long frame moves further in one sweep.

### Player controller (Main.bb:4041-4368 `MovePlayer`, 4370-4610 `MouseLook`)

- The collider is a pivot, `EntityRadius 0.15, 0.30` (an ellipsoid), `HIT_PLAYER`, sphere-pickable;
  a separate `Head` pivot, radius 0.15, falls and rolls on death (Main.bb:7966-7973, 4441-4472;
  `Kill`, Main.bb:3667-3689).
- Walking, sprinting, crouching, stamina, injuries, bloodloss, gravity, falling, footsteps by the
  floor's material (`GetStepSound` picks down 1 unit and maps the surface's texture name through
  materials.ini, Main.bb:9236-9291), heartbeat, the blink key, noclip (Main.bb:4188-4216). Numbers
  in section 5.
- Camera: placed on the collider each frame, raised by the head bob and crouch, turned by the
  smoothed mouse, shaken by `CameraShake`, zoomed by `CurrCameraZoom` (blood loss, 173 nearby),
  rolled by injuries (Main.bb:4373-4433); in the pocket dimension it rolls by
  `Sin(MilliSecs / 150) x 30` outside the safe height band (Main.bb:4435-4439).
- Wearables change it: the gas mask, hazmat, and SCP-1499 overlay `GasMaskOverlay` and refill or cap
  stamina (Main.bb:4502-4515); night vision tints `NVOverlay`, swaps the fog texture, raises room
  ambient (Main.bb:4517-4534), drains battery, and shows NPCs through walls with super NVG
  (Main.bb:11291-11417); the vest halves mouse speed (Main.bb:4424-4425); SCP-1025's six diseases
  (Main.bb:4536-4607); SCP-714 caps stamina at 10 (Main.bb:4114-4117).

### Collisions (Main.bb:202, 2697-2703)

Types: `HIT_MAP` 1, `HIT_PLAYER` 2, `HIT_ITEM` 3, `HIT_APACHE` 4, `HIT_178` 5, `HIT_DEAD` 6.

| Source | Target | Method | Response |
|---|---|---|---|
| PLAYER (player, Head, every NPC) | MAP | 2 ellipsoid to polygon | 2 slide |
| PLAYER | PLAYER | 1 ellipsoid to ellipsoid | 3 slide in XZ (no sliding down) |
| ITEM | MAP | 2 | 2 |
| APACHE | APACHE | 1 | 2 |
| 178 | MAP | 2 | 2 |
| 178 | 178 | 1 | 3 |
| DEAD | MAP | 2 | 2 |

Blitz's response (blitz3d/blitz3d/world.cpp:147-301): a swept ellipsoid, scaled to a sphere by
`radius.x / radius.y` in y; up to `MAX_HITS = 10` contacts a move, each backing off
`COLLISION_EPSILON` from the plane; slide 2 projects the remaining move onto the plane (and onto
the crease of two), keeping the rest of the distance; 3 limits the slide to the XZ distance; going
back against the move's direction stops it; squeezed between two planes, or 10 hits, puts the
entity back where it started. Collision targets are tested against their **previous** transform.
`ResetEntity` (134 calls) clears the sweep after teleports.

- **No step height or slope code.** Stairs are climbed by the 0.30 ellipsoid riding up the edges and
  by ramps in the rooms' invisible collision meshes (section 3, step 3). Gravity is a translate each
  frame, and "on the floor" means a collision more than 0.25 below the collider's center this frame
  (Main.bb:4257-4260); the player only falls when a 10-unit pick down hits something
  (`PlayerFallingPickDistance`, Main.bb:4278-4289), so it never falls into a void.
- Doors: each leaf is `HIT_MAP` (Main.bb:1922-1923), moved by `MoveEntity` while opening; a closing
  door pushes the player 0.15 out of its plane (Main.bb:2206-2220); some doors add an invisible
  `doorhit.b3d` while moving (Main.bb:2000-2022).
- NPCs: `HIT_PLAYER`, radius 0.2 mostly (NPCs.bb:87-600), gravity 0.005 per tick up to their
  `MaxGravity` 0.2, only where the player is near (NPCs.bb:5000-5095).
- Items: radius 0.01, fall 0.0004 per tick while a pick 10 down hits, stop when they touch the map
  (`EntityCollided`, Items.bb:520-541), push each other apart (Items.bb:543-569).

### Picking

`EntityPickMode` 2 (polygons) on rooms, doors, buttons, props; 1 (spheres) on the player, items,
waypoints, screens, light pivots. `EntityPick(Camera, 0.6..1.2)` for buttons, screens, levers
(Main.bb:2098, MapSystem.bb:6256, 6670); `LinePick` down for footsteps, gravity, items, decals;
`EntityVisible` for every line-of-sight test (173, NPC sight, cameras, items, light sprites,
waypoint links); `CameraPick` once. `EntityInView` (41 calls) is SCP-173's "is it seen" test
(section 5).

### NPCs (NPCs.bb)

22 types (NPCs.bb:3-7): 173, 106 (OldMan), Guard, Class-D, 372, Apache, MTF, 096, 049, 049-2
(Zombie), 513-1, 035's tentacles, 860-2, 939, 066, the pocket dimension's plane, 966, 1048-A, 1499-1,
008-1, and the clerk. `CreateNPC` (NPCs.bb:73-622) makes each from NPCs.ini's speed / 100 and
scale; `UpdateNPCs` (NPCs.bb:671-5122) is one long `Select` on type with each one's states, sounds,
and animation frames; `UpdateMTFUnit` (NPCs.bb:5239-6823) the MTF squad. Vision: `MeNPCSeesPlayer`
(NPCs.bb:5180-5218), within `8 - CrouchState + PlayerSoundVolume` units and 60 degrees, or heard;
NPCs far away teleport to waypoints nearer the player (`TeleportCloser`, NPCs.bb:5124-5167). Speeds
are x 1.1 on Euclid and x 1.2 on Keter for 173, 106, 096, 049, 939, and MTF (`NPCSpeedChange`,
NPCs.bb:7274-7286).

Pathfinding: waypoints from each .rmesh and one at every door (`InitWayPoints`,
MapSystem.bb:5861-5988) are linked when within 7 units, in the same room or through a door, under a
room's `MaxWayPointY`, and `EntityVisible` to each other, at most 5 links each. `FindPath`
(MapSystem.bb:5999-6191) is an A* over them (Manhattan heuristic in XZ, MTF paying 0.5 more for
non-door nodes), from the nearest waypoint (visible ones preferred 3:1) to the one nearest the goal,
keeping at most 20 nodes from the start (it paths again as it goes). **Remake**:
`std.algo.search`'s `AStar` (docs/stdlib.md) over the same graph; E4's `EntityVisible` builds it.

### Items and inventory (Items.bb, Main.bb `DrawGUI` 4614-7114)

133 templates (Items.bb:115-333) with model, inventory image, document image, scale, and texture;
10 slots (`MaxItemAmount`, Items.bb:3); wallets and clipboards hold 10 more (Items.bb:419-426);
picking (Items.bb:595-739), dropping (741-791), and every item's use in `DrawGUI`. SCP-914's
recipes are code (`Use914`, Main.bb:9421-10030), SCP-294's drinks Data/SCP-294.ini (`Use294`,
Main.bb:10032-10220).

### Events (UpdateEvents.bb, Main.bb:2435-2695)

73 `Case` blocks in `UpdateEvents` (UpdateEvents.bb:1-8775), one per event name or two: the intro
("173", 346), the alarm (48), the pocket dimension (1955), each SCP's chamber, the tesla gates,
elevators, tunnels, SCP-1499's dimension (8604); plus `UpdateDimension1499` (8777) and
`UpdateEndings` (9165: Gate B's "exit1" at 9170, Gate A at 9599). `InitEvents`
(Main.bb:2499-2688) places events into rooms by name, some by chance (`CreateEvent`'s probability,
e.g. 7%, plus 10% with aggressive NPCs, for SCP-106 in tunnels, Main.bb:2508). An event is 3
float states, 2 sounds and channels, a string, and an image (Main.bb:2435-2447), saved with the game.

### The map generator (MapSystem.bb:7023-7769 `CreateMap`)

`SeedRnd GenerateSeedNumber(seed)` (MapSystem.bb:7037; the seed string XORed char by char, shifted
0 to 23 bits, Main.bb:10582-10590), then on an 18x18 grid (`map size`, options.ini;
MapSystem.bb:1750): a winding corridor from the bottom middle upward with 10-15-wide horizontal runs
and 4-5 branches up (7043-7113), checkpoints where it crosses a zone line (zones by `GetZone`,
7802-7804); each cell's shape from its neighbors (1, 2, 2C, 3, 4); at least 5 dead ends, one 4-way,
and one corner per zone forced in (7148-7356); then the fixed rooms placed by rank within their zone
and shape (`SetRoom`, 7371-7453: start, roompj, 914, archive, 205, 2closets, 2testroom2, ...,
079, 106, 008, 035, coffin, 2nuke, 049, ..., exit1, gateaentrance, room1lifts, ...) and the rest by
`commonness`-weighted chance (`CreateRoom`, 2058-2097); rotations from the neighbors, some by
`Rand(2)` (7486-7563); Gate A, the pocket dimension, the intro's 173 room, and SCP-1499's dimension
placed outside the grid (7572-7584); doors on shared edges, open with chance `Max(Rand(-3,1),0)`
(7650-7734); adjacency links (7736-7767). SCP-860-1's forest (`GenForestGrid`, 877) and the
maintenance tunnels' 19x19 grid are generated the same way, and SCP-1499's 64x64 chunk grid from
the same seed (`SetChunkDataValues`, 8192). **The same seed gives the same map only with Blitz3D's
own generator** (section 6).

### Doors and elevators (Main.bb:1830-2433; MapSystem.bb:6663-6998)

Five door kinds (`dir`): 0 normal, 1 big containment door, 2 heavy, 3 elevator, 4 SCP-914's
(Main.bb:2119-2139); buttons (plain, keycard, scanner, keypad) found by a pick within 0.6 of the
camera and 0.7 of the player (Main.bb:2087-2115); auto-closing doors (1 in 8 open doors,
Main.bb:1994), timed lock-room doors with a caution siren at 110 ticks (Main.bb:2144-2151); doors
hidden beyond 30 units (`HideDistance x 2`, measured as |dx| + |dz|), re-checked every 30 ticks
(Main.bb:2044-2079). `UseDoor` (Main.bb:2252-2410) checks keycards, codes, and the remote door
control. **Elevators don't move**: a
ride waits 500 ticks (about 7 s) inside a 280-unit box with `CameraShake = Sin(|state| / 3) x 0.3`
and the elevator sound, then teleports the player, NPCs, and items inside to the other car's same
offset (MapSystem.bb:6741-6998). Levers are dragged by the mouse's y speed between -80 and 80
degrees (MapSystem.bb:6663-6718).

### Save and load (Save.bb)

Quick save (F5) on Safe anywhere, on Euclid only at clickable monitors (`SAVEONSCREENS`,
`UpdateMonitorSaving`, MapSystem.bb:6618-6661), on Keter on quit with permadeath deleting the save
(Main.bb:3167-3226, 3675-3680). Not while falling (Save.bb:6), dead, or in 173's room or at the gates.
`LoadGameQuick` reloads in place, with `QuickLoadPercent` progress and events resuming as they load
(Main.bb:3366, `DrawQuickLoading`, Menu.bb:1996).

### Menus and options (Menu.bb; Main.bb:7116-7810 `DrawMenu`)

The main menu (Menu.bb:57-1122): a flickering SCP-173 image, random glitch phrases, tabs for new game
(name, seed, intro, difficulty, custom map), load game, options, and quit, all immediate-mode
(`DrawButton`, `DrawTick`, `SlideBar`, `InputBox`, the `Slider3`-`Slider7` pickers, Menu.bb:1636-2600)
with 2D images and `AAText`, scaled by `MenuScale = GraphicHeight / 1024` (Main.bb:185). The
launcher (Menu.bb:1124-1317): a 640x480 window listing graphics modes and drivers before the game.
Options (options.ini, read at Main.bb:51-269, 1531-1544): resolution, fullscreen, borderless,
brightness (`AmbientLight`), screen gamma, vsync, frame limit, FPS display, anti-aliasing, room
lights, bump mapping, texture detail (`TextureLodBias` 0.8 to -0.8, Main.bb:89-102), particle
amount, HUD, intro, mouse sensitivity, invert, smoothing, music and sound volume, user tracks, SFX
release, console, achievement pop-ups, and key bindings (DirectInput scancodes, Main.bb:257-267,
named by KeyName.bb).

### HUD (Main.bb:4748-4800)

Blink meter and stamina meter: 204x20 outlines at (80, height-95) and (80, height-55), filled with
`(204-2) x fraction / 10` copies of a 10-pixel image, and their icons (blink, sprint or crouch) in
30x30 boxes 50 to the left, a red border while the eyes are irritated; the hand icon at the center
or toward a grabbable thing; the debug HUD. Messages fade from `MsgTimer / 2` alpha at the center,
200 pixels down (Main.bb:3268-3296).

### Achievements (Achievements.bb)

37 (`MAXACHIEVEMENTS`, Achievements.bb:3), names in achievementstrings.ini, kept in the save; a
pop-up when given (`UpdateAchievementMsg`, Achievements.bb:128-180). No platform achievements.

### The console (Main.bb:370-1530 `UpdateConsole`)

Opened with F3 when `[console] enabled` (Main.bb:3228-3240); about 60 commands (help, noclip,
godmode, teleport, spawnitem, spawn, camerafog, gamma, wireframe, playmusic, 173speed, debughud,
notarget, giveachievement, seteventstate, ...); typed through `GetKey`, scrolled by dragging.
**Remake**: game code; E11's debug UI could host it.

### Particles (Particles.bb; DevilParticleSystem.bb)

Particles.bb: sprites (`SpriteViewMode 3`, `EntityFX 1+8`) with speed, gravity x 0.004, alpha and size
change, lifetime (Particles.bb:15-82); textures 0-8 (smoke, flash, dust, ...; Main.bb:8225-8233);
emitters for steam and smoke (Particles.bb:85-197). Blend: smoke, blood, and smoke2 get 1 (alpha);
the others ask for `BLEND_ADD`, **which is never declared**, so it's 0 and Blitz picks the blend
from the texture: those particles draw alpha-blended, not added (Particles.bb:28-34;
blitz3d/blitz3d/brush.cpp:166-187). Dust motes near the camera (Main.bb:4476-4495), door dust
(Main.bb:2171-2191). Devil system: section 2.

### Lighting

- **Rooms are baked**: fullbright meshes with vertex colors, lightmaps, and diffuse textures
  (section 3). Room lights (`CreateLight 2`, point) light only what isn't fullbright: NPCs, items,
  doors, props, decals. A room's lights are shown only within 8.5 units of the camera
  (MapSystem.bb:7919-7930), and there can be 32 per room (MapSystem.bb:1601). Blitz enables every
  shown light for every lit mesh (blitz3d/gxruntime/gxscene.cpp:232-246); past the driver's limit
  of active lights (8 on most DX7 hardware) the rest light nothing, so which lights reach an NPC
  depends on creation order. Props in rooms are parented to the fullbright mesh but keep their own
  FX, so they're lit.
- `AmbientLight Brightness` (options' `brightness`, 50 by default) each frame (Main.bb:2953,
  11276-11284); 140 at the surface and in the intro (UpdateEvents.bb:1410, 9570, 9725); doubled under
  night vision (Main.bb:11277).
- Light sprites: each light's small sprite and halo; the halo shows when within 8.5 and visible,
  is rescaled **every frame** to `Rnd(0.38,0.42)`, `Rnd(0.35,0.45)`, or `Rnd(0.3,0.5)` by its random
  flicker class (the flicker), and fades with distance. Its visibility and alpha are recomputed
  only when one shared timer is 0; the timer gains `FPSfactor` for every light, every frame, and
  wraps at 8 (MapSystem.bb:7907-8072, 7917, 8042-8045). Light cones for rooms with
  `usevolumelighting` (`AddLightCones`, MapSystem.bb:8514-8542).
- Fog: linear, black, from `CameraFogNear` and `CameraFogFar` (options' 0.5 and 6.0) scaled by
  `LightVolume`, which follows the nearby lights' intensity (Main.bb:2944-2948,
  MapSystem.bb:5647-5679); per-area values in section 5. The camera's far plane is
  `Min(fogFar x LightVolume x 1.5, 28)`, so not far past the fog's end nothing is drawn.
- `LightBlink` and `SecondaryLightOn`: blackouts darken the dark overlay up to 0.9
  (Main.bb:3086-3091).

### Rendering tricks

- **Security cameras** (MapSystem.bb:6284-6616): each screen camera renders the world into a
  512x512 viewport of the back buffer every 12 ticks (`RenderInterval`), while the screen is in
  view and the eyes are open, and `CopyRect`s it into one of two 512x512 textures, shown on a sprite
  with a monitor overlay (additive). SCP-895's coffin feed swaps in gore images and drains sanity.
  room2sl's wall of monitors uses `Room2slCam` (Main.bb:8367-8371). Checkpoint monitors swap their
  texture by `PaintSurface` (MapSystem.bb:8074-8148).
- **Blur ("Dreamfilter")** (Dreamfilter.bb; Main.bb:3013-3019, 11272-11434): an orthographic camera
  (`CameraProjMode 2`) 10000 units away holds a full-screen quad with last frame's image; each frame
  renders the world, then that camera's pass with the quad at alpha `BlurVolume` (up to 0.95) over
  it, then copies the back buffer into the 2048x2048 texture: a feedback motion blur. The same
  camera carries the fog-vignette sprite (fog.jpg, multiply, `EntityOrder -1000`), the gas mask,
  infection, and night-vision overlays (-1003), and the NVG blink (-1005) (Main.bb:7885-7930).
- The **dark** and **light** overlays are sprites on the main camera at 1.0 in front
  (`EntityOrder -1002`): black at `darkA` alpha for blinking, sanity, death, and blackouts; white for
  flashes (Main.bb:7936-7964, 3023-3141).
- **Gamma** (Main.bb:3322-3351): above 1, the frame is copied into a texture and added over itself at
  alpha `gamma - 1`; below 1, multiplied by a texture cleared to `255 x gamma` grey.
- **Borderless scaling** (Main.bb:3307-3318, 11437-11494): when the window's size differs from the
  render size, the frame is copied into a 2048x2048 texture and redrawn scaled on a quad.
- Decals (Main.bb:10756-10845): sprites with `SpriteViewMode 2` placed by picks, growing and fading.
- Skyboxes (Skybox.bb): a cube mesh of six clamped textures, `EntityFX 1+8`, `EntityOrder 1000`,
  moved to the camera each frame.
- SCP-860-1's forest terrain (`load_terrain`, MapSystem.bb:7809-7900): a grid mesh from a heightmap's
  red channel (`ReadPixelFast`), with a second copy 0.01 above textured as path, its vertex alpha
  from a mask (`EntityFX 1+2+32`).
- `EntityAutoFade`, `MeshCullBox` (NPC copies, NPCs.bb:153, 171), `EntityOrder` for overlays, and
  rooms hidden by `HideEntity` beyond two rooms (`UpdateRooms`, MapSystem.bb:5631-5673) and by alpha
  0 behind closed or unseen doors (5681-5704).

### Sound (Main.bb:8923-9331; StrictLoads.bb)

- **Positional sound is computed by hand**: `PlaySound2`, `LoopSound2`, and `UpdateSoundOrigin` set
  volume `volume x (1 - distance / range) x SFXVolume` and pan `Sin(-DeltaYaw(camera, source))`, range
  10 by default (section 5). No Doppler, no occlusion, no front and back.
- Ambience: every frame a 1-in-1500 chance of a random ambient clip per zone (8 light containment, 11
  heavy, 12 entrance, 15 general, 5 pre-breach, 10 forest; Main.bb:1657-1663, 2888-2929) from a pivot
  moved within 1 unit of the camera; rooms' sound emitters loop within their range
  (MapSystem.bb:5611-5619).
- Music: one stream at a time; changing tracks fades out by `FPSfactor / 250` per tick, then in by 10%
  of the difference per tick (Main.bb:8992-9026); the zone sets `ShouldPlay` (Main.bb:2877).
- Pausing (inventory, console, menu) pauses every event, NPC, door, emitter, ambient, and breath
  channel and stream (`PauseSounds`, `ResumeSounds`, Main.bb:9028-9154).
- The radio item: its channels of broadcasts and user tracks (`RadioCHN`, Main.bb:355, 6107-6300).

### Loading screens (Menu.bb:1337-1615)

`DrawLoading(percent)` is called between load steps: a random screen from loadingscreens.ini (image,
alignment, up to several texts shown as the percentage passes each share), a 300-wide bar of
blink-meter images, "LOADING - n %", `UpdateMusic` above 20%; at 100% it waits for a key or a click
(Menu.bb:1553-1612). Loading is synchronous: each step blocks until it's drawn.

### Difficulty (Difficulty.bb)

Safe (save anywhere, easy), Euclid (save at monitors, normal), Keter (save on quit, permadeath,
aggressive NPCs, hard), Custom (each set). "Other factors" set the blink interval (section 5) and NPC
speeds; aggressive NPCs raise event chances and the teleport-closer distance
(NPCs.bb:5135, Main.bb:2508-2513).

---

## 5. Feel: the numbers

Units: Blitz world units (a room is 8; a door 312 x RoomScale = 1.22 tall and 204 x RoomScale = 0.80
wide, Main.bb:1909); "per tick" means per `FPSfactor = 1`, a 70th of a second. These are the
remake's test fixtures: each can be checked by driving the remake and the original with the same
inputs at 70 fps.

### Timing

| What | Value | Where |
|---|---|---|
| Tick | `FPSfactor = clamp(elapsed_s x 70, 0.2, 5.0)`, per frame, not fixed | Main.bb:2814-2817 |
| Pause | `FPSfactor = 0` in menus, inventory, console, screens, keypads, 294 | Main.bb:2820 |
| Frame limit | off by default; `Delay(1000 / limit - elapsed)` | Main.bb:2822-2828, options.ini |
| Vsync | on by default (`Flip 1`) | Main.bb:3355-3359 |
| Smoothing | `CurveValue(target, old, s)`: moves `(target - old) x FPSfactor / s`, never past the target; returns `old` when paused | Main.bb:10598-10606 |
| Angle smoothing | `CurveAngle`: the same on the shortest way round | Main.bb:10608-10615 |

### Movement

| What | Value | Where |
|---|---|---|
| Walk speed | 0.018 units per tick (1.26 per second) | Main.bb:4043 |
| Sprint | x 2.5 (0.045 per tick) while sprint is held, not crouched, stamina above 0 | Main.bb:4130-4134 |
| Crouch | speed / (1 + CrouchState): half when fully crouched; `CrouchState` curves to `Crouch` with s = 10 | Main.bb:4121-4125, 4198 |
| Injuries | speed / Max((injuries + 3) / 3, 1); above 0.5 also x Min(Sin(Shake/2) + 1.2, 1) (a limp) | Main.bb:4218-4221 |
| Acceleration | `CurrSpeed = CurveValue(target, CurrSpeed, 20)` while a key is held | Main.bb:4250 |
| Stopping | `CurrSpeed = Max(CurveValue(0, CurrSpeed - 0.1, 1), 0)`: immediate | Main.bb:4252 |
| Directions | 8: forward 0, back 180, diagonals 45 / 135, strafe 90; added to the collider's yaw + 90 | Main.bb:4224-4247 |
| Move | `TranslateEntity` by `Cos(angle), Sin(angle)` x speed x FPSfactor, world space | Main.bb:4255 |
| Gravity | DropSpeed -= 0.006 per tick, down to -2.0; applied x FPSfactor | Main.bb:4281, 4291 |
| On the floor | a collision below the center by more than 0.25; resets DropSpeed | Main.bb:4258-4275 |
| Landing sound | when landing faster than 0.07 per tick | Main.bb:4263 |
| Fall only over ground | only if a pick 10 units down hits | Main.bb:4278-4289 |
| Noclip | 2.0 x speed, along the camera's facing | Main.bb:318, 4200-4216 |
| Stamina drain | 0.4 x StaminaEffect per tick sprinting (100 to 0 in 250 ticks, 3.6 s); empty sets -20 | Main.bb:4132-4133 |
| Stamina regen | 0.15 / 1.25 per tick moving, 0.15 x 1.25 standing, up to 100 | Main.bb:4070-4074 |
| Breathing | under 50 stamina while sprint is held: a breath at volume Min((70 - stamina) / 70, 1); under 5 the hard breath | Main.bb:4085-4105 |

### Collision and picking

| What | Value | Where |
|---|---|---|
| Player | ellipsoid 0.15 (XZ) x 0.30 (Y), method 2, response 2 against the map | Main.bb:7967, 2697 |
| Player vs NPCs | method 1, response 3 | Main.bb:2698 |
| Head (dead) | sphere 0.15 | Main.bb:7972 |
| NPCs | 0.2 (most), 173 0.23 x 0.32 (`CollRadius` 0.32), 049-2, 966 and others in CreateNPC | NPCs.bb:88, 111, 119-600 |
| Items | 0.01, pick by sphere, within 1.2 of the camera, in view and visible | Items.bb:375-376, 504-518 |
| Buttons | within 0.7 of the player, picked within 0.6 from the camera | Main.bb:2093-2098 |
| Screens and monitors | within 1.2 / 1.0 | MapSystem.bb:6255, 6632 |
| Max contacts a move | 10 (then back to the start) | blitz3d/blitz3d/world.cpp:149, 228-231, 294-300 |

### Camera and mouse

| What | Value | Where |
|---|---|---|
| Eye height | collider + 0.6 + bob - 0.3 x CrouchState (0.9 above the feet standing, 0.6 crouched) | Main.bb:4400-4403 |
| Field of view | `CameraZoom 1.0`: 90 degrees horizontal (Blitz's frustum width is `2 x near / zoom`, blitz3d/blitz3d/camera.cpp:61-62), vertical from the aspect ratio | Main.bb:4376 |
| Zoom effect | `CameraZoom Min(1 + CurrCameraZoom / 400, 1.1)` (down to 84.5 degrees); `CurrCameraZoom` falls 1 per tick | Main.bb:4376-4377 |
| Near / far | 0.05 / `Min(fogFar x LightVolume x 1.5, 28)` | Main.bb:2948 |
| Mouse to degrees | `the_yaw = smoothedX x 0.3 / (1 + vest)`; same for pitch | Main.bb:250-251, 4424-4425 |
| Sensitivity | `MouseXSpeed() x (MouseSens + 0.6)`, MouseSens -0.5..0.5 (0 default: 0.18 degrees per pixel) | Main.bb:4408, Menu.bb:824 |
| Smoothing | `CurveValue(raw, smoothed, (6 / (MouseSens + 1)) x MouseSmooth)`; MouseSmooth 1.0 default | Main.bb:4408-4421, 269 |
| Lag spike | when `abs(FPSfactor / previous - 1) > 1`, the smoothed speeds are zeroed | Main.bb:4410-4416 |
| Pitch limit | +-70 degrees, kept in a variable, not read back | Main.bb:4428-4431 |
| Mouse source | windowed: cursor pixels (`WM_MOUSEMOVE`, with Windows' pointer acceleration), clamped to the window; fullscreen with DirectInput: raw counts | blitz3d/gxruntime/gxruntime.cpp:432-449, blitz3d/bbruntime/bbinput.cpp:101-110 |
| Re-centering | `MoveMouse` to the center once the cursor is within 250 px of the left or right or 150 px of the top or bottom | Main.bb:253-254, 4498-4500 |
| Camera shake | `Rnd(-CameraShake, CameraShake)` degrees on pitch and yaw each frame; decays 0.1 per tick | Main.bb:4373, 4433 |
| Shake sources | elevator `Sin(abs(state) / 3) x 0.3`; explosions and SCPs 1-30 (NPCs.bb:1383 SCP-096's 30, 3943 SCP-066's 10) | MapSystem.bb:6808, NPCs.bb |

### Head bob and footsteps

| What | Value | Where |
|---|---|---|
| Phase | `Shake += 7 x Min(Sprint, 1.5) x FPSfactor` (mod 720) while a move key is held | Main.bb:4154 |
| Vertical | `Sin(Shake) / (20 + 20 x CrouchState) x 0.6`: 0.03 standing, 0.015 crouched | Main.bb:4395 |
| Roll | `clamp(Sin(Shake / 2) x 2.5 x Min(injuries + 0.25, 3), -8, 8)` degrees (0.63 uninjured) | Main.bb:4396, 4433 |
| Step rate | a step each time `Shake mod 360` passes 180: every 51.4 ticks walking (0.73 s), 34.3 sprinting (0.49 s) | Main.bb:4155 |
| Step volume | `(1 - 0.6 x Crouch) x SFXVolume`; 8 random clips per set, walk and run sets, per material | Main.bb:4159-4183 |
| Noise for NPCs | `PlayerSoundVolume` 4.0 on a walking step, `2.5 - 0.6 x Crouch` on a running one, 3.0 on landing; curves to 0 with s = 5 | Main.bb:4160, 4164, 4273, 2954 |

(The walking step is the louder of the two to NPCs. It looks inverted but is what shipped.)

### Blinking

| What | Value | Where |
|---|---|---|
| Interval | `BlinkTimer` starts at `BLINKFREQ`: Safe `Rnd(490,700)`, Euclid `Rnd(455,665)`, Keter `Rnd(420,630)` | Main.bb:3059-3067 |
| Countdown | `0.6 x BlinkEffect` per tick with eyes open, so 11.7-16.7 s on Safe; irritation adds `Min(irr/100 + 1, 4)` per tick | Main.bb:3072-3073 |
| Blink | from 0 to -20 at 1 per tick (0.29 s): dark `Sin(abs(t) x 18)` from 0 to -5, full from -5 to -15, `abs(Sin(t x 18))` to -20 | Main.bb:3048-3070 |
| Key | the blink key sets the timer to 0; holding it keeps it at -10 | Main.bb:4349-4350 |
| Start | -10 on a new game | Main.bb:8516 |
| HUD | the meter shows `BlinkTimer / BLINKFREQ` | Main.bb:4756 |
| Eye drops | `EyeStuck` keeps the eyes open; blur and dark below 9000 and 6000 | Main.bb:3036-3046 |

### SCP-173 (NPCs.bb:683-933, NPCs.ini)

| What | Value |
|---|---|
| Speed | 38 / 100 = **0.38 units per tick** (26.6 per second; x 1.1 Euclid, 1.2 Keter), straight at the player while unseen |
| Seen | `EntityInView(173's mesh, Camera)` while the eyes aren't between -6 and -16 of the blink and the NVG isn't blinking (NPCs.bb:722-724); Blitz tests the mesh's box corners against the camera's frustum, near and far planes included, with **no occlusion** (blitz3d/bbruntime/bbblitz3d.cpp:1009-1023) |
| Chases | within 15 it scrapes (volume from how far it moved) and checks sight: `EntityVisible` within 10 (NPCs.bb:706-724); unseen and within 12 (`HideDistance x 0.8`) it moves straight at the player if it sees him, else to where it last saw him, else wanders, turning at random 1 in 400 a frame (NPCs.bb:788-876) |
| Kills | within 0.65 while unseen; neck snap, camera turned 80-100 degrees (NPCs.bb:829-853) |
| Far | over 50: 1 in 70 a frame, teleports to a waypoint 15-25 away on both X and Z; over 12 (`HideDistance x 0.8`): 1 in 70 a frame `TeleportCloser` (NPCs.bb:766-787, 5124-5167) |
| While seen | blur `Max((4 - d) / 6, 0.1..0.9)`, zoom up to 15 x (3.5 - d) / 3.5, horror stings within 3.5 once a minute, scrape `Rand(700)` within 1.5 (NPCs.bb:731-761) |
| Doors | 1 in 20 a frame, opens unlocked button doors within 0.5 (NPCs.bb:792-818) |
| Height | the collider is kept at or below y 0.35 (NPCs.bb:884) |

Other speeds from NPCs.ini, per tick: 106 0.012, 096 0.09, 049 0.015, 049-2 0.008, guards and MTF
0.02, 860-2 0.08, 939 0.04, 066 0.02, 966 0.01, 1499-1 0.015. Scales likewise (173 0.35 over its
mesh depth, NPCs.bb:102).

### Fog, range, and light per area

| Area | Fog near-far | Fog / clear color | Far plane | Ambient | Where |
|---|---|---|---|---|---|
| Facility | 0.5-6.0 x LightVolume | 0,0,0 | `Min(6 x LV x 1.5, 28)` | 50 (`brightness`) | Main.bb:2944-2953, options.ini |
| LightVolume | `CurveValue(TempLightVolume, LV, 50)`; Temp = `Max(sum(intensity^2 x (15 - d) / 15) / 4.5, 1)` over shown lights within 15 | | | | Main.bb:2944, MapSystem.bb:5647-5679 |
| Intro (173) | no fog | | 40, then 15 | 140 | UpdateEvents.bb:1402-1410 |
| Pocket dimension | per state | 38,55,47 (trench), 19,27,23, black | | | UpdateEvents.bb:1983-2493 |
| SCP-860-1 forest | 0.5-8.0 | 98,133,162 x Max(1 - 860-2's state / 300, 0.1), x 2 without 860-2 | RoomScale-8.5 | | UpdateEvents.bb:6520-6531 |
| SCP-1499 | 40-80 | 96,97,104 | 90 | | UpdateEvents.bb:8958-8961 |
| Gate B surface | 5-45 | 200+50a, 200+30a, 200 | 60 | 140 | UpdateEvents.bb:9562-9570 |
| Gate A | 5-30 | 200+40a, 200+20a, 200 | 30 | 140 | UpdateEvents.bb:9717-9725 |
| NVG | far kept, fog texture swapped (fogNV.jpg), room ambient 15, ambient x 2 | | | | Main.bb:4517-4534, 11276-11277 |
| Console `camerafog` | sets near and far | | | | Main.bb:1155-1156 |

Fog is linear per pixel on eye depth where the card has table fog, else per vertex
(blitz3d/gxruntime/gxscene.cpp:62-69).

### Lights

| What | Value | Where |
|---|---|---|
| Attenuation | Blitz's point light: D3D's attenuation 0 and 2 are 0, attenuation 1 is `1 / range`, so intensity `range / distance`, unbounded range | blitz3d/gxruntime/gxlight.cpp:14, 31, 40-42 |
| .rmesh light | range = file / 2000; color = file x Min(intensity x 0.8, 1) | MapSystem.bb:614-619 |
| Shown | within 8.5 of the camera | MapSystem.bb:7919-7930 |
| Intensity | `(r + g + b) / 255 / 3` | MapSystem.bb:5734 |
| Halo alpha | `Max(3 x (Brightness / 255) x (intensity / 2), 1) x (1 - clamp((d + 0.5) / 7.5, 0, 1))`; `Brightness / 255` is integer division (0 below 255), so the `Max` is 1 | MapSystem.bb:7953-7957 |
| Halo size | rescaled every frame: `Rnd(0.38,0.42)` for flicker classes 1-4 (and always in the intro), `(0.35,0.45)` for 5-9, `(0.3,0.5)` for 10; class `Rand(1,10)` | MapSystem.bb:7937-7949, 8010-8020, 5762 |
| Light update timer | `+ FPSfactor` per light per frame, wraps at 8 | MapSystem.bb:8042-8045 |
| Sprites | small 0.13 (light1.jpg, add), halo 0.6 (lightsprite.jpg, add, order -1, view mode 1, random roll) | MapSystem.bb:5736-5761 |

### Sound

| What | Value | Where |
|---|---|---|
| 2D "3D" | volume `v x (1 - d / range) x SFXVolume`, pan `Sin(-DeltaYaw(cam, src))`, range default 10 (min 1) | Main.bb:8923-8939 |
| One-shot out of range | not played at all | Main.bb:8929 |
| Loops | `LoopSound2` sets volume even past range (negative, clamped to 0) | Main.bb:8941-8967 |
| Updates | `UpdateSoundOrigin` changes nothing once out of range, so a channel keeps its last volume | Main.bb:9313-9331 |
| Defaults | `sound volume` 1.0, `music volume` 0.5 | options.ini |
| Mixer | 44100 Hz, 64 channels | FMod.bb:1-16 |
| Per sound | up to 32 channels; freed 5 s after the last stops | StrictLoads.bb:30-56, 87 |
| Music fade | out `FPSfactor / 250` per tick; in 10% of the gap per tick | Main.bb:8998, 9008 |
| Heartbeat | every `70 x 60 / rate` ticks; volume falls 0.05 per tick | Main.bb:4354-4365 |
| Door, camera, lever, elevator | `PlaySound2` ranges 10, 4.0 (camera hum), 1.0 (lever), 4.0 (elevator beep) | MapSystem.bb:6596, 6694, 6893 |
| Ambient chance | 1 in 1500 a frame | Main.bb:2888 |

### Doors

| Kind | Opening | Where |
|---|---|---|
| Normal (0) | `openstate += 2 x FPSfactor x (fast + 1)` to 180; each leaf moves `Sin(openstate) x FPSfactor / 80` (0.72 units in total) | Main.bb:2120-2123 |
| Big (1) | 0.8 per tick; `/ 180`; dust at 15 on closing | Main.bb:2124-2127, 2170-2191 |
| Heavy (2) | leaves `/ 85` and `/ 120` | Main.bb:2128-2131 |
| Elevator (3) | `/ 162` | Main.bb:2132-2135 |
| 914 (4) | 1.4 per tick, `/ 114` | Main.bb:2136-2138 |
| Closing push | the player kept 0.15 from a closing door's plane | Main.bb:2206-2220 |
| Elevator ride | 500 ticks | MapSystem.bb:6813, 6913 |

A door's travel is integrated per frame, so it ends slightly differently at different frame rates
(only the closed position snaps back, Main.bb:2227-2232).

### Overlays and effects

| What | Value | Where |
|---|---|---|
| Blur | `BlurVolume = Min(CurveValue(0, BV, 20), 0.95)`; `BlurTimer / 1000` at least | Main.bb:3013-3017 |
| Dark overlay | sprite at 1.0 in front, scaled `Max(w / 1240, 1)` x `Max(h / 960 x 0.8, 0.8)`, order -1002 | Main.bb:7941-7947 |
| Death | `KillTimer` falls 0.8 per tick to -360, dark `abs(KillTimer) / 400`, blur `abs(KillTimer) x 5` | Main.bb:3093-3105 |
| Flash | `LightFlash` falls 1/70 per tick, alpha +-0.2 noise | Main.bb:3132-3139 |
| Blackout | `LightBlink` falls 1/35 per tick; dark `LightBlink x Rnd(0.3,0.8)`; no secondary light: dark 0.9 | Main.bb:3086-3091 |
| Sanity | below -200: dark up to 0.6, heartbeat `70 + abs(sanity + 200) / 6` | Main.bb:3025-3034 |
| Bleeding | blur, a drop decal with chance `Min(injuries, 4) / 200` a frame, zoom `(Sin(ms/20) + 1) x bloodloss x 0.2` | Main.bb:4296-4339 |

---

## 6. Engine requirements

Prioritized: P0 blocks the first playable room; P1 blocks the whole game; P2 is polish or an option.
"Plan" is docs/engine.md's milestone, or **MISSING** where nothing there covers it.

| # | Pri | Requirement | Plan |
|---|---|---|---|
| 1 | P0 | A Blitz-compatible random generator: Park-Miller with multiplier 48271 (Schrage's 44488 / 3399), the float `(state & 65535) / 65536 + 0.5 / 65536`, `Rand(a, b) = Int(rnd x (b - a + 1)) + a` with a swap when b < a, `SeedRnd` masked to 31 bits (0 becomes 1), seeded 0x1234 at start (blitz3d/bbruntime/bbmath.cpp:5-54); the game reseeds from `MilliSecs` at start (Main.bb:203) and from the map seed in `CreateMap`. Same seed, same map; the generator, events, chunks, and gameplay share one stream. std.math's `Random` is PCG32 | The game's own code; E14's seeded_random proves it on FutureBlitz's integers |
| 2 | P0 | Blitz's collisions: swept ellipsoids against polygons and ellipsoids, responses 1-3, 10 contacts, epsilon backing, y-scaled radii, collision against the targets' previous transforms, `CountCollisions`, `CollisionX/Y/Z`, `EntityCollided`, `ResetEntity`; done once per `UpdateWorld` | E4 Differs on purpose, by architecture.md: targets tested where they are now, 8 iterations by default (10 by SetCollisionIterations), using them up settles the mover, riding an edge by SetStepHeight. |
| 3 | P0 | Picks and visibility: `LinePick` (with radius), `EntityPick`, `CameraPick`, `PickedX/Y/Z/NX/NY/NZ`, `PickedEntity`, `EntityPickMode` 1 and 2 with the obscurer flag, `EntityVisible` | E4 |
| 4 | P0 | `EntityInView` exactly as Blitz: the mesh's bounding box's 8 corners (or the position, for non-meshes) against the camera's 6 frustum planes, near and far included, no occlusion | E4 (the 173 test: make it a named fixture) |
| 5 | P0 | The scene: entities, pivots, hierarchy, `CopyEntity` of hierarchies (rooms, NPCs), hide and show, names, `FindChild`, `GetChild` by index in creation order, `EntityAlpha 0` meaning "not drawn but still collides and picks", every transform (`TurnEntity`, `MoveEntity`, `TranslateEntity`, `PointEntity`, `AlignToVector`, `TFormPoint`, `TFormVector`, `DeltaYaw`, `DeltaPitch`, `VectorYaw`, `EntityDistance`), Blitz's degrees and left-handed axes | E4 |
| 6 | P0 | Building meshes in code: surfaces, `AddVertex` with **two UV sets**, vertex colors and alpha, `AddTriangle`, `VertexCoords`, `UpdateNormals`, `AddMesh`, `CopyMesh`, `FlipMesh`, `PositionMesh`, `RotateMesh`, `ScaleMesh`, `MeshWidth/Height/Depth`, `MeshCullBox`; brushes and `PaintSurface`; reading back `GetSurfaceBrush`, `GetBrushTexture`, and **`TextureName`** (step sounds and monitors key off texture file names) | E4 |
| 7 | P0 | The fixed-function look: `EntityFX` 1, 2, 8, 16, 32; layered textures (4 used) with blends 1, 2, 3, and 5 and a UV set per layer, so `(2 x ambient x vertex + lightmap) x diffuse` draws as Blitz drew it; DX7 vertex lighting with point lights at `range / distance`, the ambient light, and the driver's active-light limit (usually 8); linear fog; Blitz's resolution of blend 0 from the texture's alpha; `EntityOrder`; alpha sorting; sprites with view modes 1-4 | E5 (and E4 for sprites and FX) |
| 8 | P0 | The .rmesh room format (section 3) | The game's own reader; E14's room_files sample proves what it needs, with a reader of its own |
| 9 | P0 | Window, frame timing, and input as Blitz had them: `MilliSecs` (a millisecond clock), `Flip` with and without vsync, `KeyDown`, `KeyHit`, `GetKey`, `FlushKeys` by DirectInput scancode (options.ini's binds are scancodes: 17 W, 30 A, 31 S, 32 D, 57 space, 42 left shift, 15 tab, 29 left ctrl, 63 F5, 61 F3), `MouseHit`, `MouseDown`, `MouseX/Y`, `MouseX/Y/ZSpeed`, `MoveMouse`, `HidePointer`, focus | E1 (steps 2-4). Relative mouse mode (E1 step 4) must be scaled to match cursor pixels, or mouse look feels different (section 7) |
| 10 | P0 | Several cameras in one frame and several `RenderWorld`s: viewports, `CameraProjMode` 0, 1, 2 (orthographic overlay cameras), `CameraClsMode`, `CameraClsColor`, `CameraZoom`, `CameraRange`, `CameraFog*`; hiding the main camera while another renders | E4 |
| 11 | P0 | 2D over 3D: images with a mask color (`MaskImage` 255,0,255 and 255,255,0), `DrawImage`, `DrawImageRect`, `LoadAnimImage`, `CreateImage`, `ResizeImage`, `ScaleImage`, `RotateImage`, `MidHandle`, `HandleImage`, `ImageRectOverlap`, `Rect`, `Oval`, `Line`, `Color`, `ClsColor`, `Cls`, `Text` and `StringWidth` with TrueType fonts sized like GDI's | E3 |
| 12 | P0 | Render to texture and screen copies: render a camera's 512x512 viewport and copy it into a texture (security cameras); copy the whole back buffer into a texture each frame (blur, gamma, borderless scaling); `SetBuffer TextureBuffer` and `Cls` into a texture; `CopyRect` between back buffer, images, and textures | E2's offscreen targets and E3's `CopyRect` cover parts; **copying from the back buffer and drawing into textures aren't named**. Better: let a camera render straight into a texture (one call), and keep a back-buffer copy for the blur |
| 13 | P1 | Skinned .b3d with bones, `SetAnimTime` to a fractional frame in Blitz's frame numbers, bone transforms overridable after animation (`ManipulateNPCBones` turns a bone after `UpdateWorld`) | E7 |
| 14 | P1 | Converting .b3d and .x to glTF, keeping node and bone names, frame numbering, per-surface brushes and texture names, and vertex colors | The game's own converter, as no converter is in the engine; E14's models_as_a_converter_writes_them proves the receiving side |
| 15 | P1 | Audio as Blitz had it: `LoadSound` of Ogg Vorbis, `PlaySound`, channels with `ChannelVolume`, `ChannelPan` -1..1, `ChannelPitch` in Hz, pause, resume, stop, `ChannelPlaying`, 64 channels mixed at 44100; streams with volume, pan, pause, looping | E9 |
| 16 | P1 | Loading textures as Blitz did: flags 1, 2, 8 (and mipmaps by default), 16, 32; `LoadAnimTexture` strips; JPEG, PNG, BMP, one DDS; **formats sniffed from the bytes** (`.pt`, `.it`, `.sc`, `.dc`, `.pd`, `.AIJ` images; `.cwm` sounds); Windows paths with backslashes in any case | E8 (and E5 for the flags) |
| 17 | P1 | Pixel access: `LockBuffer`, `ReadPixelFast`, `WritePixelFast` on images and textures (the forest's heightmaps and masks, the anti-aliased font atlas) | E3's CPU staging path |
| 18 | P1 | Graphics modes: the display's modes (`CountGfxModes3D`, `GfxModeWidth/Height`), fullscreen, windowed, borderless at the desktop's size; a render size different from the window's (the game scales it itself) | E1 step 1 |
| 19 | P1 | INI reading and writing | std (`std.data`'s `Ini`, docs/stdlib.md:1147-1153); nothing for the engine |
| 20 | P1 | Binary files: Blitz streams' ints, floats, bytes, and length-prefixed strings for saves and maps; files and folders (`ReadDir`, `FileType`, `CreateDir`, `DeleteFile`) | std (`std.io`'s `BinaryReader`, `BinaryWriter`, `File`, `Dir`) |
| 21 | P1 | Pathfinding over the waypoint graph | std (`std.algo.search`'s `AStar`); the graph is the game's |
| 22 | P1 | Particles: Particles.bb's and the Devil system's sprites, ported as game code | E4 (sprites); E6's GPU particles aren't needed |
| 23 | P1 | Procedural map generation | game code over item 1; nothing for the engine |
| 24 | P2 | Video: two startup AVIs with separate audio | fbengine_video (E12), from MPEG-1 the game converts them to; E14's two_videos_at_startup |
| 25 | P2 | Bump mapping: the modified runtime's environment bump (blend 6, `TextureBumpEnvMat` -0.012/0.012, offset 0.5, scale 1.0, MapSystem.bb:35-41) on rooms whose diffuse texture has a `bump` in materials.ini; on by default | LayerBlend.BUMP; E14's bump |
| 26 | P2 | `TextureLodBias` (texture detail, 0.8 to -0.8), anti-aliasing (`AntiAlias`), wireframe (console) | E2 (sampler bias, MSAA) and E5 |
| 27 | P2 | Gamma: the game's own add and multiply passes, or a real gamma or tonemap curve | E2 offscreen targets (as the game does it) or E6 |
| 28 | P2 | Text input for the console and save names (`GetKey`) | E1 step 3 |
| 29 | P2 | A debug UI for the console and debug HUD | E11 |
| 30 | drop | The updater (TCP, HTTP, zip), cpuid, FreeImage, DirectPlay, the launcher window, 16-bit mode, "enable VRAM" | none |

Converters and the .rmesh reader are the game's own, as is Blitz's random generator; E14's samples
prove what they need of the engine.

---

## 7. Risks

1. **Frame-rate dependence.** The original advances by `FPSfactor` per frame but rolls dice, sweeps
   collisions, smooths the mouse, and integrates doors once per frame (section 4, Main loop). Its
   "feel" is whatever it was at the players' frame rate, usually 60 with vsync on (options.ini), not
   70. A remake on a fixed tick changes ambience and event frequency; a remake on a variable step
   inherits the original's quirks. Recommended: run the simulation at the original's per-frame
   step but at a fixed 60 or 70 Hz, render interpolated, and test the section 5 fixtures at that
   rate.
2. **Collision feel.** The sliding along walls, catching on door frames, riding up stair edges, and
   being squeezed back to the start of a move all come from Blitz's exact swept-ellipsoid code with
   10 contacts and its epsilon (world.cpp:147-301), plus the double-sided collision copies and
   invisible ramps in .rmesh. Jolt's character controller (E10) won't feel the same; E4 must
   reproduce Blitz's algorithm, numerically close, against the same triangle soups.
   fbengine doesn't reproduce Blitz's algorithm line for line: item 2 names how it differs, and
   its proof pins each difference; the remake tunes its feel on these.
3. **SCP-173's "seen" test.** It's a frustum test of a bounding box with no occlusion, cut off at
   the camera's far plane (which shrinks with the fog to as little as 9 units) and gated by the
   blink timer between -6 and -16. Any change (a different box, a different FOV at a different
   aspect ratio, the far plane, occlusion) changes when 173 moves, the game's central mechanic.
4. **Mouse look.** Windowed Blitz reads cursor pixels with Windows' pointer acceleration and
   re-centers past 250/150 px; fullscreen reads DirectInput counts. SDL's relative mode gives raw
   counts. 0.18 degrees per pixel, the smoothing, and the lag-spike zeroing have to be tuned against
   the original on the same mouse, or it will feel off at once.
5. **The fixed-function look.** Lightmaps added over a 2x-modulated ambient layer, multiplied by the
   diffuse texture; fullbright rooms; DX7 vertex lighting on NPCs with `range / distance`
   attenuation and a limit of usually 8 active lights; Blitz's blend-0 fallback (the particles that
   ask for an undeclared `BLEND_ADD`); 8-bit color math; mipmapping and filtering as a 2008 card did it; linear
   table fog. E5 targets this, but every one is a separate fixture to match.
6. **The feedback blur and overlays.** The blur blends the previous frame at up to 0.95 every frame,
   so its look depends on frame rate and on the exact order of the world pass, the overlay camera's
   pass (fog vignette, masks), the copy, and the 2D. Copying the back buffer into a texture each
   frame must be cheap on Vulkan.
7. **Seeds.** Same seed, same map needs Blitz's generator bit for bit and the same order of every
   `Rand`/`Rnd` call through map generation, room filling (`FillRoom` rolls dice), and event
   placement, with Blitz's single-precision float rounding (`Floor(0.3 x Float(count))`). Any
   reordering in the port changes every map.
8. **Assets.** 113 .rmesh rooms, 168 .b3d and .x models, and their textures go through two converters
   that don't exist yet, and the rooms' look depends on lightmap blending flags inside the files.
   Odd extensions and backslash paths need care. The models' animations are addressed by frame
   number all through NPCs.bb and UpdateEvents.bb.
9. **Size.** 49,137 lines of game code in the 24 files, most of it NPCs.bb (7460),
   MapSystem.bb (8750), UpdateEvents.bb (10181), and Main.bb (12087), written against globals and
   Blitz's `Type` lists; about 75 events and 22 NPC types, each with hand-tuned states. E13's
   migrator does the mechanical part; checking that each event still plays the same is the long part.
10. **The modified runtime.** 1.3.11 was built with a patched Blitz3D (`ErrorLog`, `TextureLodBias`,
    environment bump mapping). Reference screenshots for E5's done-when should be taken with bump
    mapping off, or the remake needs E5 to grow a bump mode.
11. **Text.** Menus wrap and clip by GDI's metrics of Courier New, DS-Digital, and Journal at sizes
    scaled to the screen height; a different rasterizer or kerning shifts layouts.
12. **Sound.** The 2D pan-and-volume fake has no front and back and leaves channels at their last
    volume once out of range; using E9's real 3D sound would sound better and different. Keep the
    fake for fidelity, behind a setting.
