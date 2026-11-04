# The sample models' licenses

Each is from Khronos's glTF-Sample-Assets, https://github.com/KhronosGroup/glTF-Sample-Assets, at
commit 8f492390ddc7a2ae221a1369ef560589cf8c70fb, unchanged, with its own LICENSE.md beside it.

| Model | File | License | By |
| --- | --- | --- | --- |
| SimpleSkin | glTF-Embedded/SimpleSkin.gltf | CC0-1.0 | Khronos's sample assets |
| SimpleMorph | glTF-Embedded/SimpleMorph.gltf | CC0-1.0 | Khronos's sample assets |
| RiggedSimple | glTF-Binary/RiggedSimple.glb | CC-BY-4.0 | Cesium, 2017 |
| MorphPrimitivesTest | glTF-Binary/MorphPrimitivesTest.glb | CC-BY-4.0 | ft-lab, 2018 |

They're test data, never part of a package.

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

`tests/golden/khronos_*.png` are renderings of those, modified from them by being drawn, each under its
model's license. There's none of DamagedHelmet, as its 2016 original is non-commercial: it's only ever
fetched and drawn by a test, and nothing of it is kept in this repository.

`tests/golden/model_RiggedSimple_*.png` and `tests/golden/model_MorphPrimitivesTest_*.png` are
renderings of those two models, modified from them by being drawn, and are CC-BY-4.0, as they are.
