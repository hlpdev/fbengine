# Khronos's sample models against their references

E6 step 8's check of the renderer: `tests/khronos/main.fb` draws each model and holds it to what's
below. ctest runs it as `khronos.models`, and `khronos.golden` then checks the frames it saved against
`tests/golden/khronos_*.png`, so a change shows however small, all but DamagedHelmet's, whose 2016
original is non-commercial, and MultiUVTest's, which is mostly Khronos's logo, so no rendering of those is
kept here. CMake fetches the models and their reference images at configure, from a pinned commit, each
checked by its SHA-256 (`files.cmake`, `fetch.cmake`). Offline they're left out with a warning, at once, and
`FBENGINE_REQUIRE_KHRONOS`, which CI sets, tries each again after 5, 20, and 60 s before it's fatal. CI keeps
them in a cache keyed by `files.cmake`, saved once they're fetched, whatever the tests go on to do, and each
is still checked by its hash. `tests/models/LICENSES.md` has their licenses.

On Linux, `khronos.golden` lets four frames differ, as what's measured there says, since golden images are
Mesa 26.2.4's and Linux's CI has Ubuntu's 25.2.8. The same natives on 25.2.8 on Windows give what Linux x64
gives, to the pixel, so it's the driver, and it's mipmaps: 25.2.8's `vkCmdBlitImage` makes each level of an
8-bit linear format half a 255th brighter than the mean of what's under it, 2 by level 4, where 26.2.4's is
within 0.01, and both make sRGB's the same. What differs is what normal and metal-rough maps do from far off.

| Frame | Pixels more than 2 away, x64, arm64 | Most away | Let differ |
| --- | --- | --- | --- |
| AlphaBlendModeTest | 0.63%, 1.39% | 20 | 1.5% |
| NormalTangentMirrorTest | 0.15%, 0.44% | 96 | 0.5% |
| NormalTangentTest | 0.056%, 0.061% | 85 | 0.1% |
| TextureTransformMultiTest | 0.004%, 0.071% | 24 | 0.1% |

## What the checks can say

Each model's checks are of three kinds, strictest first:

- **Worked out**, by hand from glTF's specification, with none of the engine's code. `main.fb`'s Worked
  model is cells of one material each, lit by an ambient light of 1 and nothing else, with no tonemap, so
  a pixel's its base color by what reaches it plus what it emits, encoded by sRGB's formula, which `main.fb`
  has for itself. This is what can catch a color space, a factor, a channel, occlusion, or a mask. Where a
  model says what's right in pixels, as TextureTransformTest's arrows do, it's held to that too.
- **Against itself**, independent of the light: a model that tests something shows the answer beside it.
- **Against its reference**, which is broad. Each reference is the small `screenshot/screenshot.png` beside
  its model, of Khronos's sample viewer, with an environment map and a camera that aren't with them, from
  130 to 338 pixels across. So it's only how much of what's drawn in either is drawn in both, each cut to
  what's drawn and laid on a 64 by 64 grid, and how each square's brightness correlates, 1 for together.
  Correlation ignores gain and hue, and overlap's only a silhouette, so these two say little alone.

Colors are in 255ths.

## What each is held to

| Model | Check | Tolerance | It measures |
| --- | --- | --- | --- |
| Worked | each of 13 cells: emissive factor, strength, map, and map by factor; base color factor, map, and map by factor; unlit; occlusion by a map, and at half strength; a mask past, short of, and at a cutoff of 0 | at most 3 of what's worked out, each channel | 0 to 0.6 |
| TextureTransformTest | its offsets' quads each their own color | at least 100 past what isn't | 157 to 167 |
| | over each arrow, which only goes through there if it reaches its green marker | at most 40 | 1.3 to 3.1 |
| TextureTransformMultiTest | each row's UV 0 and UV 1 tiles against its Sample, mean of each pixel and channel | at most 16 | 2 to 13 |
| MultiUVTest | its logo, by UV set 1, against its color, by set 0 | at least 20 | 48 |
| TextureLinearInterpolationTest | each sphere's green against 0.5, encoded; its red | at most 4; at most 2 | 0.5; 0 |
| TextureSettingsTest | Test column's cells against Sample pass's, mean of each pixel and channel | at most 20 | 18 |
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
| | each cutoff's panel gone above its mark, against the opaque panel | at least 8 | 26 to 35 |
| | each cutoff's panel whole below it; the opaque panel, top to bottom | at most 4; at most 3 | 0 to 0.4; 1.4 |
| | the blended panel a ramp, top to bottom | at least 15 | 27 |
| EmissiveStrengthTest | its first cube's light; each cube past the one before, as `KHR_materials_emissive_strength` says | at least 60; at least 1 | 163; 12 to 26 |
| EmissiveStrengthTest, in a sun's light | how many pixels a shadow darkens by 8, and lights by 2, against none | at least 200; none | 2852; 0 |
| | how many bloom lights by 3, against none | at least 200 | 116496 |
| | Neutral against no tonemap, at its brightest cube | at least 3 | 18 |

What it measures is from lavapipe, and some margins are thin. Once CI has run them, each platform's
measures go here and the limits come from them.

## Expected differences

Against a reference, these aren't bugs, and are why its tolerances are as broad as they are:

- **The environment.** Khronos's aren't in the repository the models are, so these are lit by a sky built
  in `main.fb`, with a clear horizon, as NormalTangentTest's notes ask. Everything reflective looks
  different, MetalRoughSpheres most, whose screenshot reflects a detailed courtyard.
- **The camera.** The screenshots' aren't recorded. DamagedHelmet's turn is by eye and overlap. Each is
  framed as the viewer frames a model, by its bounds, at a field of view of 45 degrees.
- **The tonemap.** These use Khronos's PBR Neutral, and the screenshots are of various viewers and ages.
- **Post effects.** Bloom's off but for its one check above, and EmissiveStrengthTest's screenshot has a
  glow around its cubes.
- **Size and resampling.** A 130 pixel screenshot on a 64 by 64 grid against a 512 pixel frame.
- **Shadows and ambient occlusion.** The viewer's screenshots have no cast shadows, and there are none here
  but for the one check above. What a material's occlusion map does is in; SSAO isn't asked for.
- **Tangents.** A surface with none takes what a pixel's derivatives make of its UVs, as `pbr.slang` says,
  not MikkTSpace, so a normal map baked against MikkTSpace can differ slightly over a curve.
- **Clearcoat.** TextureTransformMultiTest's rows of it are dark, as the renderer has none, which that
  model counts as a pass, so they're not checked.
- **TEXCOORD_2 and past.** A surface keeps two UV sets, so a map on another is left out, said once on stderr.
