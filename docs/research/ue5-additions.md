# SCP: Containment Breach Ultimate Edition v5.5.4.1: what it adds for fbengine

What Ultimate Edition v5 (UE) needs from an engine beyond what docs/research/scpcb-requirements.md
found in the original 1.3.11. Sources: UE's code, data, and changelog in
SCRATCH/ue5/SCP - Containment Breach Ultimate Edition v5.5.4.1/, its full file list in
SCRATCH/ue5-files.txt, 1.3.11 in SCRATCH/scpcb, Blitz3D in SCRATCH/blitz3d. UE is CC BY-SA 3.0
(Game.bb:7); this file describes behavior and numbers and copies no code.

**Citations.** A bare file name is UE's: `Main.bb:4561` is UE's Source Code/Main.bb; `Game.bb`,
`Data/...`, and `Changelog_of_UE.txt` are in UE's root. `1.3.11 Main.bb:4376` is the original's.
Blitz3D's source is `blitz3d/<path>:<line>`.

**The short answer.** UE is a content mod on the same engine use as 1.3.11. It calls no Blitz3D
command family 1.3.11 didn't, uses the same userlibs with identical .decls, the same modified
runtime, and loads .rmesh with a byte-for-byte identical loader. What it adds for an engine is
small: a vertical field-of-view setting with a near plane five times closer, a 3D scene behind the
main menu (with real spot lights), a screenshot key, a third startup video, opening a URL and a
file with the OS, many more bump-mapped room textures, and more of the same overlays, sprites,
and models. Two of these aren't in docs/plan.md (section 6).

**The game build.** Game.bb includes Constants.bb and Main.bb (Game.bb:14-16), and Main.bb pulls in
the rest: 27 files, 58,409 lines (Main.bb:11-17, 68, 259, 336, 1994, 2272, 2276, 2970-2978, 3033,
9834, 11239; Map_System.bb:1-2, 670, 8755; Menu.bb:6). Game.bb also includes the two converter
tools (Game.bb:21, 26), but they can't build with the game: each starts with its own `Graphics3D`
and redefines `HIT_MAP` and `StripPath` (B3D_Converter_Main.bb:3-12 against Constants.bb:7 and
Map_System.bb:610); they are built from B3D_Converter.bb and BMP_Converter.bb and counted as tools
here, as is the Map Creator (Map Creator/Map Creator.bb, Window3D.bb, and its Source Code/). The
updater (Update.bb, Blitz_File_ZipApi.bb, zlibwapi) is gone. Draw_Portals.bb is still included
and still never called (Map_System.bb:670; nothing calls `CreateDrawPortal`).

**Scripts** (run with `python -I <script> SCRATCH`): ue5_count.py (commands and decls, against
scpcb-counts.json; writes ue5-counts.json), ue5_unknown.py (calls from a modified runtime),
ue5_files.py (the two file lists compared), ue5_fdiff.py (one Function diffed between the
versions, ignoring whitespace, case, and comments), ue5_perfile.py (commands a UE file calls that
its 1.3.11 counterpart didn't).

---

## 1. Commands

Counted as scpcb-requirements.md section 1 counted them (same tokenizer, strings, comments,
fields, and `Field` lines left out). The game calls **286** of the 536 commands (1.3.11: 297),
and all 35 .bb files 299.

**New in UE's game** (not called by 1.3.11's game):

| Command | Calls | Where | For |
|---|---:|---|---|
| `Tan` | 8 | Main.bb:4561, 9291; Menu_3D.bb:429, 447 | the field-of-view zoom (section 4, Cameras) |
| `ATan` | 4 | same lines | same |
| `BufferDirty` | 2 | Achievements.bb:21, 27 | marks achievement images changed after `ResizeImage2` copied pixels into them |
| `SaveBuffer` | 1 | Main.bb:3197 | the screenshot key (section 4, UI) |

**No longer called** (all 1.3.11 calls were the updater's, Update.bb and Blitz_File_ZipApi.bb,
unless noted): `BankSize` 50, `PokeInt` 21, `CloseTCPStream` 13, `ResizeBank` 6, `ReadAvail` 6,
`ReadBytes` 5, `WriteBytes` 5, `OpenTCPStream` 4, `CopyBank` 3, `PeekShort` 1, `ReadShort` 1,
`Hex` 1 (Blitz_Basic_Bank.bb), and three from 1.3.11 Main.bb: `SeekFile` 1, `AvailVidMem` 1,
`TotalVidMem` 1 (the debug HUD and error log). Sockets are never used now, and banks only for one
`GetClientRect` rectangle (Fullscreen_Window_Fix.bb:63-66) and the INI cache (INI_Details.bb:43).

**The modified runtime** is the same one: ue5_unknown.py finds `TextureBumpEnvMat` 8,
`TextureLodBias` 4, `TextureBumpEnvOffset` 2, `TextureBumpEnvScale` 2 (Materials.bb:11-16,
Texture_Cache.bb:32-37, Main.bb:9160, 9262, 10497, Menu.bb:733), and the "MAV-less" runtime ships
as Source Code/mavless_bin.zip. `ErrorLog()` and `CatchErrors` are no longer called; the runtime
itself writes error_log_N.txt (Changelog_of_UE.txt:1390).

**Counts that moved**, all from more of the same content (more rooms, NPCs, events, overlays, and
the 3D menu), none from a new use: `EntityX` 1000 to 1318, `PositionEntity` 808 to 1041,
`EntityParent` 366 to 558, `CopyEntity` 133 to 191, `EntityVisible` 71 to 113, `ScaleSprite` 94
to 142, `EntityAlpha` 91 to 134, `EntityOrder` 17 to 30, `CameraProjMode` 14 to 21, `RenderWorld`
12 to 18, `CameraClsMode` 4 to 8, `AmbientLight` 10 to 17, `MeshCullBox` 9 to 14, `SetAnimTime` 40
to 53, `ChannelPitch` 2 to 4. `DebugLog` fell from 276 to 2 and `WriteLine` from 31 to 5.

Per file (ue5_perfile.py), the only commands a UE file calls that its 1.3.11 counterpart didn't
are the four above, `ExecFile` in Menu.bb (section 4, UI), and, in the new Menu_3D.bb, the mesh,
light, and stream commands of its own .rmesh reader (`CreateLight` 2, `LightConeAngles` 1,
`AddMesh`, `CopyMesh`, `FlipMesh`, `VertexColor`, `VertexTexCoords`, `TextureCoords`,
`ReadFloat`, `ReadString`).

---

## 2. Libraries

Every .decls file is identical to 1.3.11's (fmod, BlitzMovie, DevilParticleSystem, FreeImage,
kernel32, user32, gdi32; zlibwapi.decls and cpuid.decls are gone). The startup check now demands
fmod.dll, FreeImage.dll, dplayx.dll, and BlitzMovie.dll (Main.bb:1-9), where 1.3.11 demanded fmod
and zlibwapi.

| Library | UE's use | Change from 1.3.11 | Engine provides |
|---|---|---|---|
| fmod.dll (FMOD 3) | streams through Strict_Loads.bb:152-298 (`StreamSound_Strict` 35 calls, 1.3.11: 20); init 44100 Hz, 64 channels (FMod.bb:1, Constants.bb:179-182) | the same 13 functions, same call sites; constants moved to Constants.bb | fbengine_audio streams (E9 step 3) |
| BlitzMovie.dll | `PlayStartupVideos` (Main.bb:14645-14713): open, size, decode to an image, play, stop, close | one call site looping over three movies instead of two (section 3) | fbengine_video (E12) |
| DevilParticleSystem | Devil_Particle_System.bb, identical to 1.3.11's apart from whitespace; templates made in `LoadEntities` (Main.bb:10505-10569), updated at most once per 70 Hz tick (Main.bb:3466-3471) | none; the gas emitter also spares night-vision wearers (Particles_System.bb:233) | game code over sprites (E4 step 10) |
| FreeImage.dll | only BMP_Converter_Main.bb:392-399 (`FI_Load`, `FI_Save`, `FI_Unload`): a tool that turns lightmap .bmp files into .png | now checked at startup, still never called by the game | drop; std.image |
| user32 / kernel32 | `api_SetWindowLong`, `api_SetWindowPos` (borderless, Main.bb:79-80, 121-122), `api_GetClientRect`, `api_GetDesktopWindow` (Fullscreen_Window_Fix.bb:64), `api_GetFocus` (pause, Main.bb:9072-9078), `GlobalMemoryStatus` (debug HUD only, Main.bb:5144) | `api_GetModuleFileName` gone with the updater; the error log no longer reads memory | E1 step 1 (borderless), step 2 (focus) |
| dplayx.dll | checked at startup (Main.bb:4); no `multiplay` command is called | now demanded | drop |
| zlibwapi.dll, cpuid.dll | gone | the updater and CPU_Details.bb are gone | none |
| 4gb_patch.exe | ships in Source Code; marks the 32-bit exe large-address-aware ("Now mod use 4 GB RAM", Changelog_of_UE.txt:1211) | new | nothing: fbengine is 64-bit only |

The Map Creator's own window code (`api_FindWindow`, `api_GetActiveWindow`, `api_ShowWindow`,
Map Creator/Source Code/Main_3D.bb:21-22, 596) is a tool's.

---

## 3. Formats

From ue5_files.py over the two file lists: UE ships 2,631 files (1.3.11: 2,116): 1,064 .ogg, 955
.png, 162 .jpg, 139 .b3d, 132 .rmesh, 51 .x, 21 .bmp, 7 .ttf, 3 .avi, 3 .cbmap2, 1 .dds, and the
same made-up extensions. 1,136 files are new to UE (753 .png, 186 .ogg, 80 .b3d, 33 .rmesh).

- **.rmesh is unchanged.** UE's `LoadRMesh` (Map_System.bb:201-609) differs from 1.3.11's
  (1.3.11 MapSystem.bb:336-753) only in the props path constant and dropped debug lines: same
  header, texture flags, two UV sets, collision and trigger meshes, and point entities, and
  spotlights still become point lights. All 114 rooms in Data/rooms.ini use .rmesh; some models
  were converted from .b3d to .rmesh (Changelog_of_UE.txt:766). The 3D menu has its own reader
  of the same format (`LoadRMesh_3DMenu`, Menu_3D.bb:766-1265), which makes real spot lights
  (section 4, Lighting). The converters handle .rmesh; nothing new for the engine.
- **Textures are mostly PNG now**: 955 .png against 162 .jpg (1.3.11: 277 and 523), from
  converting most .jpg files (Changelog_of_UE.txt:635). Lightmaps are 227 `_lm` .png files and 21
  .bmp (1.3.11: 159 and 29). Skybox faces are .png (Skybox.bb:8; 1.3.11 Skybox.bb:9 used .jpg).
  The one .dds (GFX/map/forest/detail/tree1.dds) remains. Texture flags in calls are the same
  set (1, 1+2, and the same `CreateTexture` kinds). E8 step 2 loads PNG and JPEG; the converters
  handle BMP and DDS.
- **Bump maps**: 32 room bump maps in GFX/map/bumps and 7 NPC ones in GFX/npcs/bumps, all .png
  (1.3.11: a dozen .jpg in GFX/map), named by Data/materials.ini's 45 `bump` entries. Section 4.
- **Made-up extensions** stay, with new ones: GFX/npcs/scp_173_h.pt, scp_173_ny.pt, and
  GFX/decalpd(6).dc, besides 1.3.11's .pt, .dc, .it, .s, .sc, .pd, .AIJ, and .cwm. Content
  sniffing (scpcb-requirements.md section 6, item 16) is still needed, or the converters rename.
- **Video**: three AVIs, GFX/menu/startup_Undertow.avi, startup_TSS.avi, and the new
  startup_UET.avi (Changelog_of_UE.txt:853), each silent with its sound in an .ogg of the same
  name (Main.bb:14669-14692). The converters re-encode them for E12's decoder.
- **Meshes**: 139 .b3d, 51 .x, the same two .smf files; 40 NPC and vehicle models
  (Main.bb:10101-10179, section 4, Animation). Converters.
- **Sounds**: 1,064 .ogg (1.3.11: 895), the same three .cwm. 42 music tracks (1.3.11: 34).
- **Fonts**: the same seven TrueType files and the same five sizes (Main.bb:190-196).
- **Data**: the same INI formats. rooms.ini has 114 rooms and `[room ambience]` 1 to 13 (1.3.11:
  10); loadingscreens.ini 61 screens with the same keys; achievementstrings.ini 59 entries;
  events.ini 82. `LoadMap` reads .cbmap2 and .cbmap exactly as 1.3.11 did (Save.bb:2346-2721), and
  three zone maps ship in Map Creator/Maps. Saves are a new, incompatible layout
  (Changelog_of_UE.txt:538): the game's own.

---

## 4. Engine features

Everything below was checked against 1.3.11 with ue5_fdiff.py. Unchanged, so already covered by
scpcb-requirements.md: the main loop and `FPSfactor` (Main.bb:3191-3326), `MovePlayer`
(Main.bb:4119-4552: same speeds, gravity, stamina, head bob), `MouseLook` apart from the zoom
(Main.bb:4554-4913), collisions (Main.bb:2980-2986, the same seven pairs and six types), the
.rmesh loader, `UpdateRooms`, `UpdateRoomLights`, `UpdateSecurityCams` (the same 512x512
viewport, zoom 0.8, range 0.05 to 8, `RenderInterval`), the blur (Dreamfilter.bb, identical),
gamma and borderless scaling (Main.bb:3275-3320), decals, both particle systems, sound
(`PlaySound2`, `LoopSound2`, `UpdateSoundOrigin`, `UpdateMusic`, identical in Sound_System.bb),
skyboxes, the forest terrain, elevators, `ManipulateNPCBones`, `AnimateNPC`, `Animate2`, AAText,
and per-area fog (Event_System.bb:2598-3113, 7074-7077, 9900-9907, 10503-10508, 10665-10670).

### Cameras

- **Field of view.** 1.3.11 set `CameraZoom` to 1.0 times the zoom effect (1.3.11 Main.bb:4376):
  always 90 degrees horizontal, so a wider screen saw less vertically. UE divides that by the
  tangent of half the horizontal angle that a **vertical** FOV gives at the window's aspect
  (Main.bb:4561, again in the in-game options at Main.bb:9291): the setting is a vertical angle,
  and the horizontal follows the aspect (Hor+). The zoom effect `Min(1 + CurrCameraZoom / 400,
  1.1)` still multiplies on top, narrowing the view by up to 10%. The 3D menu uses the same FOV
  (Menu_3D.bb:429). An event can still set `CameraZoom` directly for a frame
  (Event_System.bb:10913).
- **The near plane is 0.01** (Main.bb:3414, 9887; 1.3.11 Main.bb:2948, 7872: 0.05), with the far
  plane `Min(fogFar x LightVolume x 1.5, 28)` as before, and 0.05 with 15 to 120 in the events
  (Event_System.bb:2009-2014, 9903-9908). That is a far/near ratio of 2,800 in the facility.
- **The overlays moved to the overlay camera.** The dark and light overlays were sprites on the
  main camera in 1.3.11 (1.3.11 Main.bb:7941, 7958); now all 17 overlays are sprites on the
  orthographic `ark_blur_cam` (Main.bb:9902-10077), drawn in `RenderWorld2`'s second pass
  (Main.bb:14310-14312). The order numbers are 1.3.11's: fog -1000, gas-mask blur -1000, the
  red-ice, crystal, and SCP-215 static overlays -1001, dark and light -1002, the masks, NVG,
  blood, helmet, SCP-447, fire, 3-D glasses, and hazmat -1003, and the NVG blink -1005.
- **More cameras a frame in the main menu**: the 3D menu renders its room with the FOV, then
  renders again with color kept and depth cleared (`CameraClsMode 0, 1`) and the room hidden, for
  the logo and darkening sprites parented to the camera (Menu_3D.bb:424-479). A low-resolution
  path (a fifth-size viewport copied into a texture) is never used: its switch is never set.

### Rendering

- **Bump mapping covers more rooms.** The same environment-bump layer as 1.3.11 (blend 6, matrix
  -0.012 and 0.012, offset 0.5, scale 1.0, Materials.bb:8-18, Texture_Cache.bb:28-37), applied in
  layer 1 between the ambient-room texture and the lightmap when the surface's diffuse texture has
  a `bump` entry (Map_System.bb:315-323): it distorts the lightmap's lookup ("distorting the
  lightmaps", Menu.bb:2145). On by default (options.ini:17), set only from the main menu ("cannot
  be changed in-game", Menu.bb:2146), which rebuilds the 3D menu (Menu.bb:653-661).
  Data/materials.ini names 32 room bump maps and 10 NPC textures, but **NPC meshes never get
  theirs**: the bumps are loaded (Materials.bb:36-42) and looked up only by room surfaces'
  diffuse names (Map_System.bb:316) and the 3D menu's room (Menu_3D.bb:944-965, where the layers
  are bump 0, lightmap 1, diffuse 2, with no ambient layer).
- **More overlays**: 17 screen sprites (1.3.11 had 7, 1.3.11 Main.bb:7887-7958), each full-screen,
  scaled `Max(width / 1024, 1)` by `Max(height / 1024 x 0.8, 0.8)`, with blend 1, 2, or 3 and
  `EntityFX 1` (Main.bb:9902-10077); the gas-mask blur's alpha follows `GasmaskBlurTimer` up to 0.5
  (Main.bb:4732). The same mechanism as 1.3.11's, more of it.
- **Translucent skinned mesh**: SCP-457 is SCP-106's skinned model retextured and drawn at alpha
  0.25 (NPCs_System.bb:639-660), so an animated, alpha-blended NPC is sorted with the scene's
  other transparent things.
- **Security cameras' red light** blinks by swapping the camera model's texture, 800 ms of every
  1250, while the camera is in view and visible (Map_System.bb:7157-7158, 7212-7222).
- **Image scaling through the 3D pipeline**: `ResizeImage2` draws an image on a quad and copies
  the back buffer back into a new image (Main.bb:14144-14158); UE now also scales every loading
  screen by `MenuScale` this way (Menu.bb:1509; Changelog_of_UE.txt:180), besides 1.3.11's items,
  screens, and achievements.
- **Fog color from the options.** 1.3.11's main loop fogged black (1.3.11 Main.bb:2946); UE's
  sets the color from options.ini's fog r, g, b each frame (Main.bb:3412), but misspells the
  green and blue variables, so only red takes effect and green and blue are always 0. With the
  default 0, 0, 0 it's black, as before.
- **Texture detail, anti-aliasing, vsync, frame limit, gamma**: as 1.3.11 (five LOD bias levels
  0.8 to -0.8, default -0.4, Main.bb:55-66; AA applied when options are saved, Menu.bb:269,
  Main.bb:9159; frame limit 20 to 119, Main.bb:9526-9537). Anisotropy is still commented out
  (Main.bb:14141). `enable vram` now defaults on (options.ini:24), which changes nothing on a
  modern GPU.
- **Wireframe** (console) is now turned off by quick loading (Save.bb:1466;
  Changelog_of_UE.txt:176).

### Lighting

- **Spot lights, in the 3D menu only.** `LoadRMesh_3DMenu` makes .rmesh `light` entities point
  lights and `spotlight` entities real spot lights (`CreateLight 3`) with the file's pitch, yaw,
  and inner and outer cone angles (Menu_3D.bb:1059-1167), up to 10 lights, each with the small
  sprite and the halo (0.2 by 0.25, additive, view mode 1, random roll). Blitz makes these D3D7 spot
  lights with theta the inner and phi the outer full cone angle in radians and falloff 1
  (blitz3d/gxruntime/gxlight.cpp:32-34, 58-60; blitz3d/bbruntime/bbblitz3d.cpp:1085-1095). In the
  game, rooms' spotlights remain point lights.
- **The ambient light** is 255 under the fine night-vision goggles (mode 3, Main.bb:14170), as in
  1.3.11. The 3D menu sets `AmbientLight Brightness` and fog 0.1 to 6.0 (Menu_3D.bb:151-335).

### The 3D main menu (Menu_3D.bb; Changelog_of_UE.txt:832, 1235)

A room from options.ini's `game progress` (`lcz`: one of room173, lockroom, room372, room914,
room2tesla_lcz; `hcz`: room009 or room3pit; or a room by name, Menu_3D.bb:42-98), loaded from its
.rmesh, scaled by RoomScale, with props, doors, posed NPCs (`SetAnimTime` to a fixed frame on
clerk.b3d and class_d.b3d, Menu_3D.bb:225-302), Particles.bb smoke (Menu_3D.bb:531-562), alarm and
ambient sounds, and a camera that sways (`Sin` of a state, Menu_3D.bb:493, 527), behind the 2D
menu. It needs nothing the game doesn't, but it is the first thing on screen: the remake must
load a lit, fogged room with props and posed models before the menu draws.

### Collisions and picks

Unchanged: the same collision pairs (Main.bb:2980-2986), the same radii for the player
(Main.bb:10090-10091), and the same pick uses. New NPCs use the same methods: SCP-178-1's
`HIT_178`, 0.2 to 0.32 radii (NPCs_System.bb:594-830). `TeleportEntity` (Main.bb:14618-14643)
places an entity on the floor by a pick up or down, as event code.

### Animation

- **40 models preloaded** in `LoadEntities` (Main.bb:10101-10179; 1.3.11 preloaded only the guard,
  MTF, and Class-D, 1.3.11 Main.bb:8007, and loaded the rest per NPC, 1.3.11 NPCs.bb:92-250)
  and copied with `CopyEntity` per NPC (NPCs_System.bb:600-830), each copy scaled and sometimes
  retextured, with its own `SetAnimTime`: copies of a skinned hierarchy that share
  geometry and keep their own pose, materials, and alpha.
- Still `SetAnimTime` with fractional Blitz frame numbers only; no `Animate`, no blending.

### Sound

Unchanged in mechanism: the 2D fake of positional sound, FMOD streams, 44100 Hz and 64 channels,
32 channels per sound. More sounds and streams (`StreamSound_Strict` 35 calls). The default
sound volume is 0.5 (options.ini:29; 1.3.11: 1.0). Room sound emitters are still at most 8
(Constants.bb:79), with 13 ambience kinds.

### Input

The same keyboard and mouse: binds by DirectInput scancode (INI_Details.bb:333-343) plus the new
**screenshot key**, 59 (F1) by default (options.ini:48, INI_Details.bb:347). A double click is
two left clicks under 800 ms (Main.bb:3236-3240, as in 1.3.11). No joystick or gamepad call.
The cursor image is drawn by the game in fullscreen (`ScaledMouseX`, Main.bb:14580-14586).

### UI, HUD, menus, options

- **Options** (INI_Details.bb:285-404) are 1.3.11's, regrouped into `[graphics]`, `[controls]`,
  `[advanced]`, `[game]` sections, plus: `fov` (default 74), `Screenshot key`, `console version`
  (the console at the bottom of the screen, full width and 300 high, or 1.3.11's 400x500 box at
  20, 40; Main.bb:414-421), `th` (unlocks the Thaumiel difficulty), and `game progress` (the 3D
  menu's room). The FOV slider is in both options screens (Menu.bb:754-762, Main.bb:9280-9291).
- **Screenshots**: the key saves the back buffer as Screenshots/screenshot_N.png, the first
  unused N, at the top of the loop, after the last `Flip` and before `Cls`, and shows "Screenshot
  taken." (Main.bb:3193-3205). What it captures is the last frame, 3D and 2D together.
- **The launcher** opens a bug-report URL in the browser and the changelog text file in its
  default app through `ExecFile` (Menu.bb:1373-1382), and no longer checks for updates.
- **An FPS counter** in the main menu (`FPSMainMenu`, Menu.bb:1223-1229), counted once a second
  (Main.bb:3227-3232).
- **Loading screens and achievement icons scale with the resolution** (Menu.bb:1509,
  Achievements.bb:19-25).
- The Thaumiel difficulty (Difficulty.bb) forces HUD off, console off, gamma 0.9, FOV 74, and fog
  0.35 to 4.5, from inside `UpdateConsole`, so only when the console is enabled (Main.bb:390-405).
- Text shadows (credits, menus, Changelog_of_UE.txt:1268, 1328) are text drawn twice.

### Saving

The same quick save and load; saves from older versions are refused. Nothing new for the
engine.

### Video

`PlayStartupVideos` (Main.bb:14645-14713), when `play startup video` is on: for each of the three
AVIs, it reads the size, decodes into an image, streams the .ogg at the sound volume, and each
frame copies the image into a 2048x2048 texture on a quad under its own camera
(`ProjectImage`, Main.bb:14715-14738), **letterboxed to 16:9** at the window's width unless the
window is already 16:9 (Main.bb:14660-14665), with "PRESS ANY KEY TO SKIP VIDEO" in Blitz's
font 50 pixels from the bottom (Main.bb:14697). A video ends on any key or when its audio stream
stops, so the audio, not the video, decides its length (Main.bb:14699). If a video can't open,
the option is turned off in options.ini and the rest are skipped (Main.bb:14683-14690).

### Performance

UE preloads all NPC models, achievement images, and 23 overlay textures at load
(Main.bb:9902-10179) and needed the 4 GB patch on 32-bit. The debug HUD shows triangles rendered
(`TrisRendered`, Main.bb:14210, 5146) and memory (Main.bb:5144), as 1.3.11's did.

---

## 5. Feel

Only numbers that changed and bear on what the engine must support. Unchanged and still the
fixtures: `FPSfactor`, the frame limiter, vsync, walk and sprint speeds, gravity, the collider,
eye height, head bob, mouse sensitivity and smoothing (the same slider ranges, Menu.bb:901, 920),
the pitch limit, blinking, the sound mixer, the music fade, the light rules, and per-area fog.

| What | UE v5 | 1.3.11 | Where |
|---|---|---|---|
| Field of view | vertical, default 74, slider 40 to 90 in steps of 0.5, saved as a whole number; console `fov` takes any value; horizontal from the aspect | fixed 90 horizontal at any aspect | Main.bb:4561, 9280-9282, 1754-1758; Menu.bb:754-756, 1861-1866; INI_Details.bb:426; options.ini:25; 1.3.11 Main.bb:4376 |
| Horizontal angle at FOV 40 / 74 / 90 | 4:3: 51.8 / 90.3 / 106.3; 5:4: 48.9 / 86.6 / 102.7; 16:9: 65.8 / 106.5 / 121.3; 21:9: 80.7 / 120.7 / 133.6 degrees | 90 at every aspect | computed from Main.bb:4561 |
| Zoom effect | up to x1.1 narrower, on top of the FOV | the same factor alone | Main.bb:4561 |
| Near plane | 0.01 | 0.05 | Main.bb:3414, 9887; 1.3.11 Main.bb:2948 |
| Fog, facility default | 0.5 to 5.5 x LightVolume (far plane `Min(8.25 x LV, 28)`) | 0.5 to 6.0 | options.ini:63-64 |
| Fog, Thaumiel | 0.35 to 4.5, gamma 0.9 (when the console is on) | none | Main.bb:390-405 |
| Fog color | options' red, green and blue forced to 0 | black | Main.bb:3412 |
| Display default | 1280x1024 fullscreen (5:4, so 86.6 degrees horizontal) | 1280x720 windowed | options.ini:7-9 |
| Sound volume default | 0.5 | 1.0 | options.ini:29 |

The FOV is the change that matters most: SCP-173's "seen" test is `EntityInView` against the
camera's frustum (scpcb-requirements.md section 7, risk 3), so a 16:9 player at the default sees
106.5 degrees across where 1.3.11 gave 90, and 173 stays frozen over a wider arc. The frustum the
engine builds from the FOV must be the one its in-view test uses.

---

## 6. Coverage

Every engine requirement UE v5 adds, against docs/plan.md.

| # | Requirement | Plan |
|---|---|---|
| 1 | A camera's field of view set as a **vertical** angle (40 to 90, any value from the console), with the horizontal from the viewport's aspect, a zoom multiplier up to 1.1 on top, changed live from the options; `EntityInView` against that same frustum | E4 step 3 (field of view), E4 step 7 (in view). Make the API's angle vertical with Hor+ horizontal, and add a fixture: Blitz's `CameraZoom z` equals a horizontal angle of `2 x atan(1 / z)` |
| 2 | A near plane of 0.01 with far planes from 6 to 120 (ratio up to 12,000) without depth fighting | E2 step 4 (depth targets). Name the depth format in it: 32-bit float, reversed Z |
| 3 | A lit, fogged room with props, posed skinned models, particles, and sounds rendered behind the 2D main menu, then a second pass of camera-parented sprites with color kept and depth cleared | E4 steps 1-4 and 10, E5 steps 1, 3, 5, E7 step 1, E3, E9 step 2 |
| 4 | Spot lights with inner and outer cones as D3D7 lit them (theta inner, phi outer, falloff 1, Blitz's `range / distance` attenuation), up to 10 a scene, in the VERTEX model | E4 step 4 (spot lights), E5 step 3. E5 step 3's VERTEX model should state D3D7's spot falloff |
| 5 | A screenshot: the last presented frame, 3D and 2D, read back and saved as PNG | E2 step 4 (reading pixels back) and E3 step 4 (the window into std.image's Image) |
| 6 | Opening a URL in the browser and a file in its default app | **MISSING**: add `OpenURL(url)` (SDL3's `SDL_OpenURL`, which also opens files) to E1 step 7 |
| 7 | Three startup videos, each with its sound in a separate OGG stream, letterboxed to 16:9, ending on a key or when the audio ends, with text over them | E12 steps 1-2. E12 step 2 should also play a video with no sound of its own, timed beside a separate fbengine_audio stream, and report its end |
| 8 | Environment bump mapping on rooms: a layer of du/dv offsets (from a bump PNG) perturbing the next layer's (the lightmap's) UVs by a 2x2 matrix (-0.012, -0.012, 0.012, 0.012), with offset 0.5 and scale 1.0; 32 room bump maps, on by default, set from the main menu | **MISSING** (as scpcb-requirements.md section 6, item 25, now on many more rooms). Add a bump-environment layer to E5 step 2's material layers (a specialization constant: the layer's texels offset the next layer's UVs), with a golden image of a bumped, lightmapped wall |
| 9 | 17 full-screen overlay sprites on one orthographic camera, orders -1000 to -1005, blends alpha, multiply, and add, alpha changed per frame | E4 step 10 (sprites), E5 step 4 (order groups, blends), E5 step 5 (orthographic overlays) |
| 10 | An alpha-blended skinned mesh sorted with the scene's transparent objects | E5 step 4 with E7 step 1 |
| 11 | Copies of a preloaded skinned hierarchy that share its geometry and keep their own pose, scale, materials, and alpha (40 models) | E4 step 1 (copy with children, sharing geometry), E4 step 6 (materials cloned on write), E7 step 1 |
| 12 | Scaling an image to the screen through a draw into a target and back into an image (loading screens, icons) | E3 step 2 (drawing into a texture, scale), E3 step 4 (copying regions) |
| 13 | Texture LOD bias in five steps, anti-aliasing on and off, vsync, and a frame limit, changed from the options without restarting | E2 step 3 (LOD bias), E2 step 4 (MSAA, recreating targets), E2 step 1 (vsync), E1 step 2 (frame timing) |
| 14 | Fog color per camera set each frame | E4 step 4 |
| 15 | A texture swapped on an entity each frame (the camera light) | E4 step 6 |
| 16 | A triangle count for the debug HUD (1.3.11 had it too, unlisted) | E2 step 6 (timings): add triangles and draws per frame to its counters |

Two MISSING, both small: E1 step 7's `OpenURL`, and E5 step 2's bump-environment layer (which
was already missing for 1.3.11 and now covers far more of the game). Nothing in UE v5 needs E6,
E10, or E11 beyond what 1.3.11 did, and nothing needs a gamepad.

**What section 6 of scpcb-requirements.md lists that UE v5 no longer needs**:

- Item 30's updater is gone from UE itself, not just dropped by the remake: no Update.bb, no
  Blitz_File_ZipApi.bb, no zlibwapi.dll, no TCP streams or banks (section 1), no cpuid.dll.
  FreeImage and DirectPlay are still demanded at startup (Main.bb:2-4) but never called by the
  game; drop both.
- `ErrorLog()` from the modified runtime is no longer called (the runtime writes the crash log);
  FutureBlitz's own stack traces and E1 step 7's `ShowMessage` cover it.
- `AvailVidMem` and `TotalVidMem` (the debug HUD) are gone.

Everything else in section 6 still applies: the Blitz random generator for seeded maps (item 1;
`SeedRnd GenerateSeedNumber` at Map_System.bb:7831, 9051, 9077, Event_System.bb:3982,
Menu.bb:434), Blitz's collisions, picks, and `EntityInView` (items 2-4), the scene and mesh
building (5-6), the fixed-function look with blends 1, 2, 3, 5 and the ambient-room layer (7),
.rmesh (8, for the converters), input by scancode (9), several cameras (10), 2D (11), render to
texture and back-buffer copies (12), skinned .b3d with Blitz frame numbers (13-14), audio (15),
texture loading with sniffing (16), pixel access (17), graphics modes (18), INI and binary files
(19-20), pathfinding (21), particles (22), the map generator (23), video (24, now three), bump
mapping (25), LOD bias and AA (26), gamma (27), text input (28), and the debug UI (29).
