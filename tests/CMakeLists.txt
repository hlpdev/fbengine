FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.16.1
  GIT_SHALLOW TRUE
  SYSTEM
)
FetchContent_Declare(
  VulkanHeaders
  GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
  GIT_TAG v1.4.350
  GIT_SHALLOW TRUE
  SYSTEM
)
FetchContent_Declare(
  volk
  GIT_REPOSITORY https://github.com/zeux/volk.git
  GIT_TAG 1.4.350
  GIT_SHALLOW TRUE
  SYSTEM
)
FetchContent_MakeAvailable(Catch2 VulkanHeaders volk)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

# ctest puts the toolchain's DLLs (MinGW's libstdc++ and friends) on PATH for the tests, so
# they run from any terminal.
set(fbengine_test_dll_paths "")
if(MINGW)
  cmake_path(GET CMAKE_CXX_COMPILER PARENT_PATH fbengine_test_dll_paths)
endif()

# Vulkan with no window, on the system's loader, which volk loads: the GPU's own driver on a
# developer's machine, and lavapipe in CI.
add_executable(fbengine_gpu_tests gpu/vulkan_tests.cpp)
target_link_libraries(fbengine_gpu_tests PRIVATE volk Vulkan::Headers Catch2::Catch2WithMain)
fbengine_set_warnings(fbengine_gpu_tests)
catch_discover_tests(fbengine_gpu_tests TEST_PREFIX "gpu."
                     DL_PATHS "${fbengine_test_dll_paths}")

# The packages' own tests, run by `fb test`, and the examples, run by `fb run` and checked against
# what they print, each after the DLLs are built into the packages' native/<target>/ folders.
# CI's GPU jobs build without them, and without fb.
option(FBENGINE_FB_TESTS "Test the packages and the examples with fb" ON)
if(FBENGINE_FB_TESTS)
  find_program(FBENGINE_FB fb REQUIRED DOC "The fb that builds and tests the packages")

  # Every package's own tests, with SDL's offscreen video, so no test opens a window, as each test
  # asks too.
  file(GLOB fbengine_packages CONFIGURE_DEPENDS LIST_DIRECTORIES true
       "${PROJECT_SOURCE_DIR}/packages/*")
  foreach(package IN LISTS fbengine_packages)
    if(EXISTS "${package}/project.toml")
      cmake_path(GET package FILENAME name)
      add_test(NAME ${name}.package COMMAND "${FBENGINE_FB}" test "${package}")
      set_tests_properties(${name}.package PROPERTIES ENVIRONMENT
                           "SDL_VIDEO_DRIVER=offscreen;FBENGINE_TEST_FONTS=${harfbuzz_SOURCE_DIR}/test/api/fonts")
    endif()
  endforeach()

  # fb's own lines, building the program, come before what it prints.
  add_test(NAME examples.hello
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/hello")
  set_tests_properties(examples.hello PROPERTIES
                       PASS_REGULAR_EXPRESSION "Running hello[^\n]*\nfbengine ${PROJECT_VERSION}\r?\n$")

  # Input's cases that need a process of their own: what panics, and what's set before the video
  # starts, each checked against what it prints.
  set(fbengine_input_cases
      "key-out-of-range|panic: Simulate[.]KeyDown[(]9999[)]: SDL has no such key"
      "button-out-of-range|panic: Simulate[.]MouseDown[(]33[)]: a mouse's buttons are 1 to 32"
      "draws-composition|draws False\r?\ndraws True\r?\ndraws True\r?\npanic: who draws the composition is set before"
      "key-name-before-video|A A\r?\npanic: headless is set before"
      "rumble-negative|panic: Rumble: a rumble lasts 0 seconds or more, not -1"
      "rumble-nan|panic: RumbleTriggers: a rumble lasts 0 seconds or more, not"
      "joystick-sizes|panic: Simulate[.]ConnectJoystick: a joystick has 0 to 255 axes, buttons, and hats, not 0, 256, and 0"
      "pads-before-start|before a Flip True\r?\neach connects at the first True\r?\nand once True"
      "show-message|Missing file: rooms.ini isn't there\r?\nwent on"
      "show-message-offscreen|Missing file: rooms.ini isn't there\r?\nwent on"
      "binding-key-out-of-range|panic: Binding[.]OfKey[(]9999[)]: SDL has no such key"
      "binding-button-out-of-range|panic: Binding[.]OfMouseButton[(]0[)]: a mouse's buttons are 1 to 32"
      "binding-nan-scale|panic: Binding[.]OfGamepadAxis: a scale is a finite number, not"
      "bind-unchecked|panic: Action[.]Bind: a key binding's code is 1 to 511, not 9999"
      "bind-y-infinite|panic: Action[.]BindY: a binding's scale is a finite number, not Inf"
      "replace-overflows|panic: Action[.]Replace: a binding's scale is a finite number, not Inf"
      "mask-threshold|panic: Mask[.]FromImage: a threshold is 1 to 255, not 0"
      "mask-region-outside|panic: Mask[.]FromImage: Rect[(]2, 0, 3, 4[)] isn't whole pixels in the image, 4 by 4"
      "mask-region-fraction|panic: Mask[.]FromImage: Rect[(]0[.]5, 0, 2, 2[)] isn't whole pixels in the image"
      "mask-pixels-short|panic: Mask[.]FromPixels: 2 by 1 RGBA8 pixels are 8 bytes, not 4"
      "mask-empty-cell|panic: Mask[.]Frames: a cell is 1 pixel or more across and down, not 0 by 4")
  foreach(case IN LISTS fbengine_input_cases)
    string(REPLACE "|" ";" case "${case}")
    list(GET case 0 name)
    list(GET case 1 printed)
    add_test(NAME input.${name}
             COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/tests/input" -- ${name})
    set_tests_properties(input.${name} PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen"
                         PASS_REGULAR_EXPRESSION "${printed}" TIMEOUT 120)
  endforeach()

  add_test(NAME examples.controls
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/controls")
  set_tests_properties(examples.controls PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen"
                       PASS_REGULAR_EXPRESSION "Running controls[^\n]*\nmove [(]0, -1[)], jump True\r?\nmove [(]1, 0[)], jump True\r?\n$")

  add_test(NAME examples.menu
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/menu")
  set_tests_properties(examples.menu PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen"
                       PASS_REGULAR_EXPRESSION "Running menu[^\n]*\noptions open: True\r?\nvolume 0[.]65, fullscreen True, name Ada\r?\nmenu again: True\r?\nplaying: True, focus none\r?\na jump: score 100\r?\nconsole: [[]> give 50, gave 50[]], score 150, typed \" \"\r?\nclosed, a jump: score 250\r?\n$")
endif()

# The scene, through fbengine's C API, with no window or GPU.
add_executable(fbengine_scene_tests scene/world_tests.cpp scene/transform_tests.cpp
               scene/camera_tests.cpp scene/light_tests.cpp scene/geometry_tests.cpp
               scene/material_tests.cpp scene/bvh_tests.cpp scene/collision_tests.cpp
               scene/handles_tests.cpp scene/pick_tests.cpp scene/sprite_tests.cpp
               scene/plane_tests.cpp scene/draw_tests.cpp
               scene/terrain_tests.cpp
               "${PROJECT_SOURCE_DIR}/native/fbengine/scene/bvh.cpp")
target_link_libraries(fbengine_scene_tests PRIVATE fbengine_native Catch2::Catch2WithMain)
fbengine_set_warnings(fbengine_scene_tests)
catch_discover_tests(fbengine_scene_tests TEST_PREFIX "scene."
                     DL_PATHS "${fbengine_test_dll_paths}" "${PROJECT_BINARY_DIR}/native/fbengine")

# The scene's panics, each a case of tests/scene/panics run on its own, which has to panic saying
# what it does.
if(FBENGINE_FB_TESTS)
  set(fbengine_scene_panics
      "freed-entity=SetName on a Pivot that was freed"
      "freed-with-parent=Name on a Pivot that was freed"
      "new-under-freed=New on a Pivot that was freed"
      "freed-world=Roots on a World that was freed"
      "current-freed=SetCurrent on a World that was freed"
      "free-main=Free on the main World: the main World stays"
      "own-parent=SetParent on a Pivot: an entity can't be its own parent"
      "under-itself=SetParent on a Pivot: an entity can't go under what's under it"
      "child-out-of-range=Child\\(2\\) on a Pivot with 2 children"
      "data-freed=Data on a Pivot that was freed"
      "poses-count=SetPoses takes a pose for each entity, not 0 for 1"
      "poses-freed=Poses on a Pivot that was freed"
      "into-nothing=TransformPoint: the entity to transform into is scaled to nothing on an axis"
      "turn-freed=Turn on a Pivot that was freed"
      "point-at-freed=PointAt on a Pivot that was freed"
      "distance-freed=DistanceTo on a Pivot that was freed"
      "yaw-freed=YawTo on a Pivot that was freed"
      "pitch-freed=PitchTo on a Pivot that was freed"
      "field-of-view=SetFieldOfView on a Camera: a field of view is between 0 and Pi, not 0"
      "range=SetRange on a Camera: a camera's range is 0 < near < far, finite, not 5 to 1"
      "viewport=SetViewport on a Camera: a viewport has a finite place and a size above 0"
      "no-window=Project on a Camera: the camera's viewport has no size: open the window, or give it a viewport"
      "horizontal-no-window=HorizontalFieldOfView on a Camera: the camera's viewport has no size"
      "camera-freed=SetOrder on a Camera that was freed"
      "intensity=SetIntensity on a Light: a light's intensity is 0 or more, not -1"
      "light-range=SetRange on a Light: a light's range is above 0, not 0"
      "cone=SetCone on a Light: a spot light's cone is 0 <= inner <= outer <= Pi / 2"
      "fog-range=SetFogRange on a Camera: a fog's range is 0 <= start < end, finite, not 5 to 5"
      "fog-density=SetFogDensity on a Camera: a fog's density is 0 or more"
      "light-freed=SetColor on a Light that was freed"
      "segments=Sphere: a shape has 3 to 100 segments, not 2"
      "triangle-index=AddTriangle on a Geometry: the surface's vertices are 0 to 2, not 5"
      "triangle-count=AddTriangles on a Geometry: triangles take indices three at a time, not 4"
      "surface-index=Surface\\(1\\) on a Geometry with 1 surface"
      "vertex-index=Vertex on a Geometry: the surface's vertices are 0 to 2, not 3 at "
      "uv-set=VertexUv on a Geometry: a vertex's UV sets are 0 and 1, not 2"
      "fit-backward=Fit on a Geometry: a box to fit into has its least corner at or below its greatest, not \\(1, 0, 0\\) to \\(0, 1, 1\\)"
      "geometry-freed=AddSurface on a Geometry that was freed"
      "mesh-freed-geometry=SetGeometry on a Geometry that was freed"
      "shininess=SetShininess on a Material: a material's shininess is 0 to 1, not 2"
      "alpha-cutoff=SetAlphaCutoff on a Material: an alpha cutoff is 0 to 1, not -1"
      "layer=SetLayer on a Material: a material's layers are 0 to 7, not 8"
      "layer-uv-set=SetLayer on a Material: a layer's UV set is 0 or 1, not 2"
      "material-alpha=SetAlpha on a Material: a material's alpha is 0 to 1, not 2"
      "color-alpha=SetColor on a Material: a material's alpha is 0 to 1, not 1.5"
      "mesh-alpha=SetAlpha on a Mesh: a mesh's alpha is 0 to 1, not -1"
      "material-freed=SetOrder on a Material that was freed"
      "surface-freed-material=SetMaterial on a Material that was freed"
      "collision-group=SetCollisionGroup on a Pivot: a collision group is 0 to 1023, not 1024"
      "collision-radius=SetCollisionRadius on a Pivot: collision radii are above 0, not 0 and 1"
      "collision-box=SetCollisionBox on a Pivot: a collision box has its least corner at or below its greatest"
      "step-height=SetStepHeight on a Pivot: a step height is 0 or more, not -1"
      "response-group=SetCollisionResponse on a World: group 0 is no group, which collides with nothing"
      "iterations=SetCollisionIterations on a World: a move touches 1 to 64 times, not 0"
      "skin=SetCollisionSkin on a World: a collision skin is 0 to 0.1, not 1"
      "collide-freed=Collide on a World that was freed"
      "pick-radius=Pick on the main World: a pick's radius is 0 or more, not -1"
      "pick-freed=Pick on a Pivot that was freed"
      "camera-pick-no-window=Pick on a Camera: the camera's viewport has no size"
      "max-slope=SetMaxSlope on a Pivot: a max slope is 0 to Pi / 2, not 2"
      "drawn-blend=DrawnBlend on a Mesh: the geometry's surfaces are 0 to 0, not 99"
      "screen-ray-freed=ScreenRay on a Camera that was freed"
      "screen-ray-no-window=ScreenRay on a Camera: the camera's viewport has no size"
      "view-frustum-freed=ViewFrustum on a Camera that was freed"
      "view-frustum-no-window=ViewFrustum on a Camera: the camera's viewport has no size"
      "camera-pick-freed=Pick on a Camera that was freed"
      "sprite-scale=SetSpriteScale on a Sprite: a sprite's size is 0 or more, finite, not -1 by 1"
      "sprite-freed=SetHandle on a Sprite that was freed"
      "tween-range=TweenedMatrix on a Pivot: a tween is 0 to 1, not 2"
      "view-tween-range=TweenedViewMatrix on a Camera: a tween is 0 to 1, not -1"
      "capture-freed=Capture on a World that was freed"
      "fixed-step-rate=FixedStep: a rate is above 0 steps a second, not 0"
      "fixed-step-most=FixedStep: a frame takes at most 1 step or more, not 0"
      "fixed-step-steps=Steps on a FixedStep: a frame takes 0 seconds or more, finite, not -1"
      "fixed-step-infinite=Steps on a FixedStep: a frame takes 0 seconds or more, finite, not Inf"
      "plane-segments=SetSegments on a GroundPlane: a ground plane has 1 to 16 segments, not 17"
      "auto-fade=SetAutoFade on a Pivot: a fade is 0 <= start < end, finite, or 0 and 0 for none, not 5 to 1"
      "mirror-freed=Reflection on a Mirror that was freed"
      "intersects-freed=Intersects on a Mesh that was freed"
      "draws-tween=Draws on a Camera: a tween is 0 to 1, not 2"
      "draws-no-window=Draws on a Camera: the camera's viewport has no size"
      "draw-projection-no-window=DrawProjection on a Camera: the camera's viewport has no size"
      "cull-box=SetCullBox on a Geometry: a cull box has its least corner at or below its greatest"
      "terrain-size=SetSize on a Terrain: a terrain is 1 to 4096 squares a side, not 0"
      "terrain-corner=SetHeight on a Terrain: a terrain's corners are 0 to 4 across and along, not \\(5, 0\\)"
      "terrain-heights=SetHeights on a Terrain: a terrain 2 squares a side has 9 heights, not 2")
  foreach(case IN LISTS fbengine_scene_panics)
    string(FIND "${case}" "=" split)
    string(SUBSTRING "${case}" 0 ${split} name)
    math(EXPR split "${split} + 1")
    string(SUBSTRING "${case}" ${split} -1 message)
    add_test(NAME scene.panics.${name}
             COMMAND "${FBENGINE_FB}" --color never run "${CMAKE_CURRENT_SOURCE_DIR}/scene/panics"
                     -- ${name})
    set_tests_properties(scene.panics.${name} PROPERTIES PASS_REGULAR_EXPRESSION "panic: ${message}")
  endforeach()
endif()

# fbengine_physics' C++ that's subtle, apart from its DLL.
add_executable(fbengine_physics_tests physics/handles_tests.cpp)
target_include_directories(fbengine_physics_tests PRIVATE
                           "${PROJECT_SOURCE_DIR}/native/fbengine_physics")
target_link_libraries(fbengine_physics_tests PRIVATE Catch2::Catch2WithMain)
fbengine_set_warnings(fbengine_physics_tests)
catch_discover_tests(fbengine_physics_tests TEST_PREFIX "physics."
                     DL_PATHS "${fbengine_test_dll_paths}")

# fbengine_physics' steps, through its DLL, which the tests find where CMake built it.
add_executable(fbengine_physics_step_tests physics/step_tests.cpp)
target_link_libraries(fbengine_physics_step_tests PRIVATE fbengine_physics_native
                      Catch2::Catch2WithMain)
fbengine_set_warnings(fbengine_physics_step_tests)
catch_discover_tests(fbengine_physics_step_tests TEST_PREFIX "physics."
                     DL_PATHS "${fbengine_test_dll_paths};$<TARGET_FILE_DIR:fbengine_physics_native>")

# fbengine_physics' examples, checked against what they print.
if(FBENGINE_FB_TESTS)
  add_test(NAME examples.falling_boxes
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/falling_boxes")
  set_tests_properties(examples.falling_boxes PROPERTIES PASS_REGULAR_EXPRESSION
                       "\n200 boxes fell, 200 came to rest, 100 on others\r?\n$")
endif()

# Shapes made in a process before any world, as shape_test's are when it runs alone.
if(FBENGINE_FB_TESTS)
  add_test(NAME fbengine_physics.shapes_alone
           COMMAND "${FBENGINE_FB}" test "${PROJECT_SOURCE_DIR}/packages/fbengine_physics"
                   --filter shape_test)
endif()

# What fbengine_physics panics for, each by a run of tests/physics/panics, which says it.
if(FBENGINE_FB_TESTS)
  foreach(case IN ITEMS "step-nan;not nan" "step-infinity;not inf" "step-huge;not 1e\\+30"
                        "step-freed;this PhysicsWorld was freed")
    list(GET case 0 name)
    list(GET case 1 said)
    add_test(NAME physics.panics.${name}
             COMMAND "${FBENGINE_FB}" --color never run "${CMAKE_CURRENT_SOURCE_DIR}/physics/panics"
                     -- ${name})
    set_tests_properties(physics.panics.${name} PROPERTIES PASS_REGULAR_EXPRESSION
                         "panic: PhysicsWorld\\.Step: [^\n]*${said}")
  endforeach()
endif()

# The job system, built into the tests from its own source, since the DLL exports only its C API.
add_executable(fbengine_jobs_tests jobs/jobs_tests.cpp
               "${PROJECT_SOURCE_DIR}/native/fbengine/jobs/jobs.cpp")
target_include_directories(fbengine_jobs_tests PRIVATE "${PROJECT_SOURCE_DIR}/native/fbengine")
target_link_libraries(fbengine_jobs_tests PRIVATE Catch2::Catch2WithMain)
fbengine_set_warnings(fbengine_jobs_tests)
catch_discover_tests(fbengine_jobs_tests TEST_PREFIX "jobs."
                     DL_PATHS "${fbengine_test_dll_paths}")

# res://'s paths, folders, and paks, built into the tests from their own sources.
add_executable(fbengine_vfs_tests vfs/vfs_tests.cpp
               "${PROJECT_SOURCE_DIR}/native/fbengine/vfs/vfs.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/vfs/folder.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/vfs/pak.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/vfs/disk.cpp")
target_include_directories(fbengine_vfs_tests PRIVATE "${PROJECT_SOURCE_DIR}/native/fbengine")
target_link_libraries(fbengine_vfs_tests PRIVATE Catch2::Catch2WithMain)
fbengine_set_warnings(fbengine_vfs_tests)
catch_discover_tests(fbengine_vfs_tests TEST_PREFIX "vfs."
                     DL_PATHS "${fbengine_test_dll_paths}")

# A program that awaits jobs before its first Flip, which the main scheduler's ticks hand over.
if(FBENGINE_FB_TESTS)
  add_test(NAME examples.loading
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/loading")
  set_tests_properties(examples.loading PROPERTIES TIMEOUT 120
                       PASS_REGULAR_EXPRESSION "Running loading[^\n]*\nloading\r?\nloaded\r?\n$")
endif()

# The loaders' decoders, built into the tests from their own sources, with libktx's writer and
# encoder, which make the KTX2 files the tests read, and stb_image_write, the PNGs and JPEGs.
add_executable(fbengine_assets_tests assets/images_tests.cpp assets/models_tests.cpp
               assets/files.cpp
               "${PROJECT_SOURCE_DIR}/native/fbengine/assets/image.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/assets/gltf.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/assets/stb.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/assets/loads.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/vfs/vfs.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/vfs/folder.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/vfs/pak.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/vfs/disk.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/jobs/jobs.cpp")
target_include_directories(fbengine_assets_tests PRIVATE "${PROJECT_SOURCE_DIR}/native/fbengine")
# Hostile files the reviews found, which mustn't decode.
target_compile_definitions(fbengine_assets_tests PRIVATE
                           FBENGINE_ASSETS_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/assets/fixtures")
# stb_image_write's code, C as it's written, without fbengine's C++ warnings.
add_library(fbengine_stb_write STATIC assets/stb_write.c)
target_link_libraries(fbengine_stb_write PUBLIC fbengine_stb)
target_link_libraries(fbengine_assets_tests PRIVATE Catch2::Catch2WithMain fbengine_stb_write ktx
                      fastgltf::fastgltf)
fbengine_set_warnings(fbengine_assets_tests)
# libktx is UCRT's here, as the DLLs are (cmake/assets.cmake).
if(MINGW)
  target_link_options(fbengine_assets_tests PRIVATE -mcrtdll=ucrt)
endif()
catch_discover_tests(fbengine_assets_tests TEST_PREFIX "assets."
                     DL_PATHS "${fbengine_test_dll_paths}")

# What Assets' mounts panic for, each by a run of tests/assets/panics, which says it, at the line
# that called the mount.
if(FBENGINE_FB_TESTS)
  foreach(case IN ITEMS "files-empty;have '', which isn't a file's path"
                        "files-folder;have 'textures/', which isn't a file's path"
                        "files-above;have '\\.\\./x\\.txt', which isn't a file's path"
                        "files-colon;have 'a:b\\.txt', which isn't a file's path"
                        "files-device;have 'maps/con\\.txt', which isn't a file's path"
                        "files-file-and-folder;files mounted at res://mf can't be"
                        "files-place;data://mf"
                        "folder-place;goes above res://")
    list(GET case 0 name)
    list(GET case 1 said)
    add_test(NAME assets.panics.${name}
             COMMAND "${FBENGINE_FB}" --color never run "${CMAKE_CURRENT_SOURCE_DIR}/assets/panics"
                     -- ${name})
    set_tests_properties(assets.panics.${name} PROPERTIES PASS_REGULAR_EXPRESSION
                         "panic: [^\n]*${said}[^\n]* at [^\n]*assets/panics/main\\.fb:")
  endforeach()
endif()

# fbengine's app area apart from its DLL: what it shows nothing for, and what a link opens, on
# Windows read back through the shell; it opens nothing.
add_executable(fbengine_app_tests app/app_tests.cpp "${PROJECT_SOURCE_DIR}/native/fbengine/app/link.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/app/languages.cpp")
target_include_directories(fbengine_app_tests PRIVATE "${PROJECT_SOURCE_DIR}/native/fbengine")
target_link_libraries(fbengine_app_tests PRIVATE SDL3::SDL3-static Catch2::Catch2WithMain)
if(WIN32)
  target_link_libraries(fbengine_app_tests PRIVATE shlwapi)
endif()
fbengine_set_warnings(fbengine_app_tests)
catch_discover_tests(fbengine_app_tests TEST_PREFIX "app."
                     DL_PATHS "${fbengine_test_dll_paths}")

# The cooker's C++, built into the tests from its own sources, which read back what it makes with
# libktx, which is UCRT's here, as the DLLs are, and with the engine's own glTF loader.
add_executable(fbengine_tools_tests tools/cook_tests.cpp tools/model_tests.cpp assets/files.cpp
               "${PROJECT_SOURCE_DIR}/native/fbengine_tools/cook/glb.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine_tools/cook/image.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine_tools/cook/model.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine_tools/cook/stb.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/assets/gltf.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/assets/image.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/jobs/jobs.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/vfs/disk.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/vfs/folder.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/vfs/pak.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/vfs/vfs.cpp")
target_include_directories(fbengine_tools_tests PRIVATE
                           "${PROJECT_SOURCE_DIR}/native/fbengine_tools"
                           "${PROJECT_SOURCE_DIR}/native/fbengine")
target_compile_definitions(fbengine_tools_tests PRIVATE
                           FBENGINE_ASSETS_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/assets/fixtures")
target_link_libraries(fbengine_tools_tests PRIVATE Catch2::Catch2WithMain fbengine_stb_write ktx
                      fastgltf::fastgltf meshoptimizer)
fbengine_set_warnings(fbengine_tools_tests)
if(MINGW)
  target_link_options(fbengine_tools_tests PRIVATE -mcrtdll=ucrt)
endif()
catch_discover_tests(fbengine_tools_tests TEST_PREFIX "tools."
                     DL_PATHS "${fbengine_test_dll_paths}")

# Typed references: a module tests/tools/references writes of its files/, which a game builds and
# prints from, and which a game that names a file that isn't there doesn't build with.
if(FBENGINE_FB_TESTS)
  set(fbengine_references_games "${CMAKE_CURRENT_BINARY_DIR}/references_games")
  add_test(NAME tools.references.write
           COMMAND "${FBENGINE_FB}" --color never run "${CMAKE_CURRENT_SOURCE_DIR}/tools/references"
                   -- "${fbengine_references_games}"
                   "${CMAKE_CURRENT_SOURCE_DIR}/tools/references/files"
                   "${CMAKE_CURRENT_SOURCE_DIR}/tools/references/trees")
  set_tests_properties(tools.references.write PROPERTIES FIXTURES_SETUP fbengine_references
                       PASS_REGULAR_EXPRESSION "\nwritten\r?\n$")
  add_test(NAME tools.references.game
           COMMAND "${FBENGINE_FB}" --color never run "${fbengine_references_games}/game")
  set_tests_properties(tools.references.game PROPERTIES FIXTURES_REQUIRED fbengine_references
                       PASS_REGULAR_EXPRESSION
                       "\nres://logo\.png\r?\nres://Textures/wall\.png\r?\nres://Textures/Walls/stone\.png\r?\n$")
  add_test(NAME tools.references.typo
           COMMAND "${FBENGINE_FB}" --color never run "${fbengine_references_games}/typo")
  set_tests_properties(tools.references.typo PROPERTIES FIXTURES_REQUIRED fbengine_references
                       PASS_REGULAR_EXPRESSION "error: Textures has no member named 'Wal'")
  foreach(tree IN ITEMS case1 case3 m1 m2)
    add_test(NAME tools.references.tree.${tree}
             COMMAND "${FBENGINE_FB}" --color never run "${fbengine_references_games}/tree-${tree}")
    set_tests_properties(tools.references.tree.${tree} PROPERTIES
                         FIXTURES_REQUIRED fbengine_references PASS_REGULAR_EXPRESSION
                         "\nbuilt\r?\n$")
  endforeach()
endif()

# fbengine_audio's own C++, where it's subtle: what's in its headers, and miniaudio's backends.
add_executable(fbengine_audio_tests audio/pool_tests.cpp audio/backends_tests.cpp
               audio/channels_tests.cpp audio/outputs_tests.cpp)
target_include_directories(fbengine_audio_tests PRIVATE "${PROJECT_SOURCE_DIR}/native/fbengine_audio")
target_link_libraries(fbengine_audio_tests PRIVATE fbengine_miniaudio Catch2::Catch2WithMain)
fbengine_set_warnings(fbengine_audio_tests)
catch_discover_tests(fbengine_audio_tests TEST_PREFIX "audio."
                     DL_PATHS "${fbengine_test_dll_paths}")

# fbengine_audio_native's C API, with a thread of the test's own mixing, as a device's does, and on
# Windows, with OLE, on the null backend.
add_executable(fbengine_audio_native_tests audio/native_tests.cpp audio/memory_tests.cpp)
target_link_libraries(fbengine_audio_native_tests PRIVATE fbengine_audio_native
                      Catch2::Catch2WithMain)
target_compile_definitions(fbengine_audio_native_tests PRIVATE
                           FBE_AUDIO_TEST_DATA="${PROJECT_SOURCE_DIR}/packages/fbengine_audio/tests/data")
if(WIN32)
  target_sources(fbengine_audio_native_tests PRIVATE audio/ole_tests.cpp)
  target_link_libraries(fbengine_audio_native_tests PRIVATE ole32 psapi)
endif()
fbengine_set_warnings(fbengine_audio_native_tests)
catch_discover_tests(fbengine_audio_native_tests TEST_PREFIX "audio."
                     DL_PATHS "${fbengine_test_dll_paths};$<TARGET_FILE_DIR:fbengine_audio_native>")

# fbengine_audio_native's C API with each of a call's allocations failing in turn: its sources built
# into the test, whose operator new fails the one it says.
file(GLOB fbengine_audio_oom_sources CONFIGURE_DEPENDS
     "${PROJECT_SOURCE_DIR}/native/fbengine_audio/*/*.cpp")
add_executable(fbengine_audio_oom_tests audio/oom_tests.cpp ${fbengine_audio_oom_sources})
target_include_directories(fbengine_audio_oom_tests PRIVATE
                           "${PROJECT_SOURCE_DIR}/native/fbengine_audio")
target_compile_definitions(fbengine_audio_oom_tests PRIVATE FBE_AUDIO_BUILDING
                           FBE_VERSION="${PROJECT_VERSION}")
target_link_libraries(fbengine_audio_oom_tests PRIVATE fbengine_miniaudio Catch2::Catch2WithMain)
fbengine_set_warnings(fbengine_audio_oom_tests)
catch_discover_tests(fbengine_audio_oom_tests TEST_PREFIX "audio."
                     DL_PATHS "${fbengine_test_dll_paths}")

# Playing a sound to miniaudio's null backend, as a game plays to its device; and ending with the
# audio started, as a program does and as a panic does, which ends at once, never waiting on its
# device's thread.
if(FBENGINE_FB_TESTS)
  add_test(NAME examples.sound
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/sound")
  set_tests_properties(examples.sound PROPERTIES TIMEOUT 60
                       PASS_REGULAR_EXPRESSION "Running sound[^\n]*\nplayed 0.25 seconds of tone.ogg\r?\n$")
  add_test(NAME examples.audio_exit
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/audio_exit")
  set_tests_properties(examples.audio_exit PROPERTIES TIMEOUT 60
                       PASS_REGULAR_EXPRESSION "Running audio_exit[^\n]*\nending with the audio started\r?\n$")
  add_test(NAME examples.audio_panic
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/audio_panic")
  set_tests_properties(examples.audio_panic PROPERTIES TIMEOUT 60
                       PASS_REGULAR_EXPRESSION "panic: a mistake, with the audio started at [^\n]*\n.*exited with code 101")
  # 64 channels at 44100 Hz under a game's load, within the budget.
  add_test(NAME examples.audio_stress
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/audio_stress")
  set_tests_properties(examples.audio_stress PROPERTIES TIMEOUT 120
                       PASS_REGULAR_EXPRESSION "600 frames on no device, up to 64 channels at once\r?\n.*within the budget\r?\n$")
endif()

# fbengine_native's frame, each test in a process of its own, as a program's first frames are.
add_executable(fbengine_core_tests core/frame_tests.cpp)
target_link_libraries(fbengine_core_tests PRIVATE fbengine_native Catch2::Catch2WithMain)
fbengine_set_warnings(fbengine_core_tests)
catch_discover_tests(fbengine_core_tests TEST_PREFIX "core."
                     DL_PATHS "${fbengine_test_dll_paths};$<TARGET_FILE_DIR:fbengine_native>")
# text's atlas and layouts, as the canvas reads them, from its sources and core's, on HarfBuzz's
# test fonts.
file(GLOB fbengine_text_sources CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/native/fbengine/text/*.cpp")
add_executable(fbengine_text_tests text/text_tests.cpp ${fbengine_text_sources}
               "${PROJECT_SOURCE_DIR}/native/fbengine/core/core.cpp")
target_include_directories(fbengine_text_tests PRIVATE "${PROJECT_SOURCE_DIR}/native/fbengine")
target_compile_definitions(fbengine_text_tests PRIVATE FBE_BUILDING
                           FBE_VERSION="${PROJECT_VERSION}"
                           FBE_TEST_FONTS="${harfbuzz_SOURCE_DIR}/test/api/fonts")
target_link_libraries(fbengine_text_tests PRIVATE SDL3::SDL3-static freetype fbengine_harfbuzz
                      fbengine_sheenbidi Catch2::Catch2WithMain)
fbengine_set_warnings(fbengine_text_tests)
catch_discover_tests(fbengine_text_tests TEST_PREFIX "text."
                     DL_PATHS "${fbengine_test_dll_paths}")

# collide's masks against a brute force, from its sources and core's.
add_executable(fbengine_collide_tests collide/collide_tests.cpp
               "${PROJECT_SOURCE_DIR}/native/fbengine/collide/collide.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine/core/core.cpp")
target_include_directories(fbengine_collide_tests PRIVATE "${PROJECT_SOURCE_DIR}/native/fbengine")
target_compile_definitions(fbengine_collide_tests PRIVATE FBE_BUILDING
                           FBE_VERSION="${PROJECT_VERSION}")
target_link_libraries(fbengine_collide_tests PRIVATE SDL3::SDL3-static Catch2::Catch2WithMain)
fbengine_set_warnings(fbengine_collide_tests)
catch_discover_tests(fbengine_collide_tests TEST_PREFIX "collide."
                     DL_PATHS "${fbengine_test_dll_paths}")

# fbengine_ui's panics, each by a run of tests/ui, which says it.
if(FBENGINE_FB_TESTS)
  set(fbengine_ui_cases
      "add-twice|panic: Node[.]Add: Node \"child\" is in another Node already"
      "add-into-itself|panic: Node[.]Add: Node \"outer\" can't go in itself"
      "insert-out-of-range|panic: Node[.]Insert: 1 isn't 0 to 0"
      "fixed-negative|panic: Sizing[.]Fixed: a length is 0 or more, not -1"
      "fill-zero|panic: Sizing[.]Fill: a weight is over 0, not 0"
      "fraction-past-one|panic: Sizing[.]Fraction: a fraction is 0 to 1, not 1[.]5"
      "style-empty-name|panic: Theme[.]SetFields: \"button[.]\" isn't a style's name"
      "set-width-unchecked|panic: Node[.]SetWidth: a fill's weight is over 0, not 0"
      "min-size-nan|panic: Node[.]SetMinSize: a least is 0 or more, and finite, not NaN"
      "padding-negative|panic: Node[.]SetPadding: padding is 0 or more, and finite, not -1"
      "margin-infinite|panic: Node[.]SetMargin: a margin is finite, not Inf"
      "anchors-backward|panic: Node[.]SetAnchors: a Max is at its Min or after"
      "style-negative-border|panic: Theme[.]SetFields: a style's widths, radii, and sizes are 0 or more, not -1"
      "style-nan-font-size|panic: Theme[.]SetFields: a style's widths, radii, and sizes are 0 or more, not NaN"
      "style-negative-padding|panic: Theme[.]SetFields: a style's padding is 0 or more, not -2"
      "max-size-negative|panic: Node[.]SetMaxSize: a most is 0 or more, or Inf, not -1 by 0"
      "gap-infinite|panic: Node[.]SetGap: a gap is 0 or more, and finite, not Inf"
      "content-size-nan|panic: Node[.]SetContentSize: a size is 0 or more, and finite, not NaN by 0"
      "anchor-nan|panic: Node[.]SetAnchors: an anchor is finite, not NaN"
      "slider-range-backward|panic: Slider[.]SetRange: a range is finite, and ends at its start or after, not 2 to 1"
      "slider-range-too-wide|panic: Slider[.]SetRange: a range is finite, and ends at its start or after, not -3"
      "slider-step-negative|panic: Slider[.]SetStep: a step is 0 or more, not -1"
      "max-length-negative|panic: TextField[.]SetMaxLength: a most is 0, for none, or more, not -1"
      "wheel-step-zero|panic: ScrollView[.]SetWheelStep: a step is over 0, not 0"
      "scale-zero|panic: Ui[.]SetScale: a scale is over 0, not 0"
      "command-with-a-space|panic: Console[.]Register: \"give item\" isn't a command's name"
      "console-no-lines|panic: Console[.]SetMaxLines: a console keeps 1 line or more, not 0"
      "debug-row-two-kinds|panic: DebugPanel[.]Switch: the debug row mode is a Label"
      "strings-fall-back-to-themselves|panic: Strings[.]SetFallback: en's strings fall back to fr's, which fall back to them")
  foreach(case IN LISTS fbengine_ui_cases)
    string(REPLACE "|" ";" case "${case}")
    list(GET case 0 name)
    list(GET case 1 printed)
    add_test(NAME ui.${name}
             COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/tests/ui" -- ${name})
    set_tests_properties(ui.${name} PROPERTIES PASS_REGULAR_EXPRESSION "${printed}" TIMEOUT 120)
  endforeach()
endif()
