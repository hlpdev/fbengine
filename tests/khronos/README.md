# Khronos's sample models against their references

E6 step 8's check of the renderer: `tests/khronos/main.fb` draws each model and holds it to what's
below. ctest runs it as `khronos.models`, and `khronos.golden` then checks the frames it saved against
`tests/golden/khronos_*.png`, so a change shows however small, all but DamagedHelmet's, whose 2016 original
is non-commercial, so no rendering of it's kept here. CMake fetches the models and their
reference images at configure, from a pinned commit, each checked by its SHA-256
(`tests/CMakeLists.txt`). `tests/models/LICENSES.md` has their licenses.

## What the references can say

Each reference is the small `screenshot/screenshot.png` beside its model in glTF-Sample-Assets. They're
of Khronos's sample viewer, with an environment map and a camera that aren't with them, and they run from
130 to 338 pixels across. So they can't be compared pixel by pixel, and nothing here does. Each model's
checks are of two kinds:

- **Against itself**, which is the strict kind, and is independent of the light: a model that tests
  something shows the answer beside it.
- **Against its reference**, which is broad: how much of what's drawn in either is drawn in both, each cut
  to what's drawn and laid on a 64 by 64 grid, and how each square's brightness correlates with the
  reference's, 1 for together. A screenshot with no plain background (MetalRoughSpheres') or that's all
  model (TextureSettingsTest's) has only the correlation.

Colors are in 255ths.

## What each is held to

| Model | Check | Tolerance | It measures |
| --- | --- | --- | --- |
| TextureSettingsTest | Test column's cells against Sample pass's, mean of each | at most 6 | 3.8 |
| | a tick cell's white, a clamp cell's green | at least 150, at most 60 | 199, 0 |
| | brightness against its reference | at least 0.85 | 0.96 |
| NormalTangentTest | each normal map against the geometry beside it, mean of each pixel | at most 10 | 4.4 to 6.2 |
| | drawn in both; brightness against its reference | at least 0.85; at least 0.2 | 0.91; 0.35 |
| NormalTangentMirrorTest | each normal map, and its mirrored ones, against the geometry | at most 14 | 4.6 to 10.4 |
| MetalRoughSpheres | a smooth metal's reflected horizon against a rough one's, and against a smooth non-metal's, how sharp | at least 3 times | 13, 13 |
| | brightness against its reference | at least 0.5 | 0.57 |
| MetalRoughSpheresNoTextures | a smooth metal's reflected horizon against a rough one's | at least 3 times | 18 |
| DamagedHelmet | drawn in both; brightness against its reference, turned 40 degrees and tipped 10 as its screenshot is | at least 0.8; at least 0.35 | 0.86; 0.42 |
| | its visor's emissive ring against what's beside it | at least 10 | 24 |
| AlphaBlendModeTest | drawn in both; brightness against its reference | at least 0.9; at least 0.5 | 0.97; 0.62 |
| EmissiveStrengthTest | its first cube's light; each cube past the one before, as `KHR_materials_emissive_strength` says | at least 60; at least 1 | 163; 12 to 26 |

What it measures is from lavapipe.

## Expected differences

Against a reference, these aren't bugs, and are why its tolerances are as broad as they are:

- **The environment.** Khronos's aren't in the repository the models are, so these are lit by a sky built
  in `main.fb`, with a clear horizon, as NormalTangentTest's notes ask. Everything reflective looks
  different, MetalRoughSpheres most, whose screenshot reflects a detailed courtyard.
- **The camera.** The screenshots' aren't recorded. DamagedHelmet's turn is by eye and overlap. Each is
  framed as the viewer frames a model, by its bounds, at a field of view of 45 degrees.
- **The tonemap.** These use Khronos's PBR Neutral, and the screenshots are of various viewers and ages.
- **Post effects.** E6 step 4's bloom isn't on here, and EmissiveStrengthTest's screenshot has a glow around
  its cubes. Bloom's own tests are gpu's.
- **Size and resampling.** A 130 pixel screenshot on a 64 by 64 grid against a 512 pixel frame.
- **Shadows and ambient occlusion.** The viewer's screenshots have no cast shadows, and there are none here.
  What a material's occlusion map does is in; SSAO isn't asked for.
