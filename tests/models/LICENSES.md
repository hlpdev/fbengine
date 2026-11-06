# The sample models' licenses

Each is from Khronos's glTF-Sample-Assets, https://github.com/KhronosGroup/glTF-Sample-Assets, at
commit 8f492390ddc7a2ae221a1369ef560589cf8c70fb, unchanged, with its own LICENSE.md beside it.

| Model | File | License | By |
| --- | --- | --- | --- |
| SimpleSkin | glTF-Embedded/SimpleSkin.gltf | CC0-1.0 | Khronos's sample assets |
| SimpleMorph | glTF-Embedded/SimpleMorph.gltf | CC0-1.0 | Khronos's sample assets |
| RiggedSimple | glTF-Binary/RiggedSimple.glb | CC-BY-4.0 | Cesium, 2017 |
| MorphPrimitivesTest | glTF-Binary/MorphPrimitivesTest.glb | CC-BY-4.0 | ft-lab, 2018 |
| AnimatedMorphCube | glTF-Binary/AnimatedMorphCube.glb | CC0-1.0 | Microsoft, 2017 |
| Fox | glTF-Binary/Fox.glb | CC0-1.0, its model; CC-BY-4.0, its rigging and animation; CC-BY-4.0, its glTF | PixelMannen, 2014, its model; tomkranis, 2014, its rigging and animation; @AsoboStudio and @scurest, 2017, its conversion to glTF |

They're test data, never part of a package.

CesiumMan, the other skinned character of Khronos's, isn't used: its texture is Cesium's logo, a
trademark, and Fox is a skinned character of several clips too.

What E6 step 8 draws (`tests/khronos/`) is fetched by CMake at configure, not kept here, at that same
commit, each with its `screenshot/screenshot.png` where it's held to one, under its model's license:

| Model | File | License | By |
| --- | --- | --- | --- |
| DamagedHelmet | glTF-Binary/DamagedHelmet.glb | CC-BY-4.0, on CC-BY-NC-4.0 | ctxwing, 2018, rebuilt from theblueturtle_, 2016 |
| MetalRoughSpheres | glTF-Binary/MetalRoughSpheres.glb | CC-BY-4.0 | Ed Mackey, Analytical Graphics, 2017 |
| MetalRoughSpheresNoTextures | glTF-Binary/MetalRoughSpheresNoTextures.glb | CC0-1.0 | Krill Gavrilov, 2019 |
| NormalTangentTest | glTF-Binary/NormalTangentTest.glb | CC0-1.0 | Ed Mackey, Analytical Graphics, 2018 |
| NormalTangentMirrorTest | glTF-Binary/NormalTangentMirrorTest.glb | CC-BY-4.0 | Ed Mackey, Analytical Graphics, 2018 |
| TextureSettingsTest | glTF-Binary/TextureSettingsTest.glb | CC-BY-4.0 | Ed Mackey, Analytical Graphics, 2017 |
| AlphaBlendModeTest | glTF-Binary/AlphaBlendModeTest.glb | CC-BY-4.0 | Ed Mackey, Analytical Graphics, 2018 |
| EmissiveStrengthTest | glTF-Binary/EmissiveStrengthTest.glb | CC-BY-4.0 | Ed Mackey, AGI, 2022 |
| TextureTransformTest | glTF/TextureTransformTest.gltf and its files | CC0-1.0 | Microsoft |
| TextureTransformMultiTest | glTF-Binary/TextureTransformMultiTest.glb | CC-BY-4.0 | Ed Mackey |
| MultiUVTest | glTF-Binary/MultiUVTest.glb | CC-BY-4.0, and Khronos's logo, a trademark | Hilo 3D |
| TextureLinearInterpolationTest | glTF-Binary/TextureLinearInterpolationTest.glb | CC0-1.0 | lexaknyazev |

`tests/golden/khronos_*.png` are renderings of those, modified from them by being drawn, each under its
model's license. There's none of DamagedHelmet, as its 2016 original is non-commercial, or of MultiUVTest, as it's
mostly Khronos's logo: they're only ever fetched and drawn by a test, and nothing of them is kept in
this repository.

`tests/golden/model_RiggedSimple_*.png` and `tests/golden/model_MorphPrimitivesTest_*.png` are
renderings of those two models, modified from them by being drawn, and are CC-BY-4.0, as they are.
`tests/golden/model_Fox_*.png` are renderings of Fox, modified from it by being drawn, under its
licenses: CC0-1.0 for PixelMannen's model, and CC-BY-4.0 for tomkranis's rigging and animation and
@AsoboStudio's and @scurest's glTF.
