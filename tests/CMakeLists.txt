FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.16.1
  GIT_SHALLOW TRUE
  SYSTEM
)
FetchContent_MakeAvailable(Catch2)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

# ctest puts the toolchain's DLLs (MinGW's libstdc++ and friends) on PATH for the tests, so
# they run from any terminal.
set(fbengine_test_dll_paths "")
if(MINGW)
  cmake_path(GET CMAKE_CXX_COMPILER PARENT_PATH fbengine_test_dll_paths)
endif()

# The tests' Vulkan: lavapipe, Mesa's driver on the CPU, whose frames are the same on every
# machine, as the only driver the loader sees. On Windows it's mesa-dist-win's MSVC build, CI's,
# fetched; on Linux the system's (apt's mesa-vulkan-drivers), each of its manifests, as a multiarch
# system has one for each architecture and the loader skips the others; on macOS it's MoltenVK,
# the system's. FBENGINE_VULKAN_DRIVER names another.
set(FBENGINE_VULKAN_DRIVER "" CACHE FILEPATH
    "The Vulkan driver manifest (an ICD .json) the tests use, alone; empty for lavapipe")
set(fbengine_vulkan_driver "${FBENGINE_VULKAN_DRIVER}")
if(fbengine_vulkan_driver STREQUAL "" AND WIN32)
  FetchContent_Declare(
    lavapipe
    URL https://github.com/pal1000/mesa-dist-win/releases/download/26.2.4/mesa3d-26.2.4-release-msvc.7z
    URL_HASH SHA256=351fc8c8b695878ffb3eaa044b3ead08672a48b1a045e3c3e3975811df0f6695
    SOURCE_SUBDIR none
  )
  FetchContent_MakeAvailable(lavapipe)
  set(fbengine_vulkan_driver "${lavapipe_SOURCE_DIR}/x64/lvp_icd.x86_64.json")
elseif(fbengine_vulkan_driver STREQUAL "" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  file(GLOB fbengine_lavapipes "/usr/share/vulkan/icd.d/lvp_icd*.json")
  list(JOIN fbengine_lavapipes ":" fbengine_vulkan_driver)
endif()
# Windows' loader takes only backslashes.
set(fbengine_vulkan_environment "")
if(NOT fbengine_vulkan_driver STREQUAL "")
  cmake_path(NATIVE_PATH fbengine_vulkan_driver fbengine_vulkan_driver)
  set(fbengine_vulkan_environment "VK_DRIVER_FILES=${fbengine_vulkan_driver}")
endif()

# stb's PNGs, for the golden image harness; stb has no releases, so it's pinned to a commit.
FetchContent_Declare(
  stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG 003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5
  SOURCE_SUBDIR none
)
FetchContent_MakeAvailable(stb)

# Vulkan with no window, on the system's loader, which volk loads, and the tests' driver; and the
# GPU through fbengine's C API and its insides, built in from fbengine_internal, with the golden
# image harness (tests/golden/) and the tests' own shaders. Each test's tags are its labels, so
# `ctest -L golden` runs the golden image tests, as tools/golden.py does.
add_executable(fbengine_gpu_tests gpu/vulkan_tests.cpp gpu/present_tests.cpp
               gpu/pipeline_tests.cpp gpu/texture_tests.cpp gpu/target_tests.cpp
               gpu/buffer_tests.cpp gpu/graph_tests.cpp gpu/canvas_tests.cpp
               gpu/pixels_tests.cpp gpu/text_tests.cpp
               golden/golden.cpp golden/golden_tests.cpp golden/stb.cpp)
target_link_libraries(fbengine_gpu_tests PRIVATE volk Vulkan::Headers Catch2::Catch2WithMain
                      fbengine_internal)
target_include_directories(fbengine_gpu_tests SYSTEM PRIVATE "${stb_SOURCE_DIR}")
target_include_directories(fbengine_gpu_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(fbengine_gpu_tests PRIVATE
                           FBE_TEST_FONTS="${harfbuzz_SOURCE_DIR}/test/api/fonts"
                           FBE_OWN_TEST_FONTS="${CMAKE_CURRENT_SOURCE_DIR}/fonts"
                           FBENGINE_GOLDEN_IMAGES="${CMAKE_CURRENT_SOURCE_DIR}/golden"
                           FBENGINE_GOLDEN_FAILURES="${PROJECT_BINARY_DIR}/golden-failures"
                           FBENGINE_GOLDEN_UPDATES="${PROJECT_BINARY_DIR}/golden-updates"
                           FBENGINE_TEST_SCRATCH="${PROJECT_BINARY_DIR}/test-scratch")
fbengine_set_warnings(fbengine_gpu_tests)
fbengine_add_shaders(TARGETS fbengine_gpu_tests NAMESPACE fbe::test_shaders
                     HEADER test_shaders.hpp
                     SHADERS gpu/shaders/triangle.slang gpu/shaders/textured.slang
                             gpu/shaders/shape.slang gpu/shaders/points.slang)
catch_discover_tests(fbengine_gpu_tests TEST_PREFIX "gpu." ADD_TAGS_AS_LABELS
                     DL_PATHS "${fbengine_test_dll_paths}"
                     PROPERTIES ENVIRONMENT "${fbengine_vulkan_environment}")

# A golden image update takes only a passing test's images: the hidden test that fails after its
# check, run updating, has to leave its image unwritten.
add_test(NAME gpu.golden_updates_only_from_passing_tests
         COMMAND "${CMAKE_COMMAND}" "-DTESTS=$<TARGET_FILE:fbengine_gpu_tests>"
                 "-DIMAGE=${CMAKE_CURRENT_SOURCE_DIR}/golden/failed_update.png"
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/golden/failed_update.cmake")
set_tests_properties(gpu.golden_updates_only_from_passing_tests PROPERTIES
                     LABELS "gpu"
                     ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}")
if(fbengine_test_dll_paths)
  set_property(TEST gpu.golden_updates_only_from_passing_tests PROPERTY ENVIRONMENT_MODIFICATION
               "PATH=path_list_prepend:${fbengine_test_dll_paths}")
endif()

# The packages' own tests, run by `fb test`, and the examples, run by `fb run` and checked against
# what they print, each after the DLLs are built into the packages' native/<target>/ folders.
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
                           "SDL_VIDEO_DRIVER=offscreen;FBENGINE_TEST_FONTS=${harfbuzz_SOURCE_DIR}/test/api/fonts;FBENGINE_OWN_TEST_FONTS=${CMAKE_CURRENT_SOURCE_DIR}/fonts")
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
      "mask-region-overflowing|panic: Mask[.]FromImage: Rect[(]2147483520, 0, 2147483520, 4[)] isn't whole pixels"
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

  # Their --watch, which a person watches in a window (tools/show.py), walked here offscreen, a step
  # each frame.
  add_test(NAME examples.controls_watch
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/controls"
                   -- --watch --frames 30)
  set_tests_properties(examples.controls_watch PROPERTIES
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       PASS_REGULAR_EXPRESSION "\nwatched 30 frames, moved True, jumped True\r?\n$")
  add_test(NAME examples.menu_watch
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/menu"
                   -- --watch --frames 60)
  set_tests_properties(examples.menu_watch PROPERTIES
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       PASS_REGULAR_EXPRESSION "\nwatched 60 frames: options True, playing True, console True, menu again True\r?\n$")

  add_test(NAME examples.menu
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/menu")
  set_tests_properties(examples.menu PROPERTIES
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       PASS_REGULAR_EXPRESSION "Running menu[^\n]*\noptions open: True\r?\nvolume 0[.]65, fullscreen True, name Ada\r?\nmenu again: True\r?\nplaying: True, focus none\r?\na jump: score 100\r?\nconsole: [[]> give 50, gave 50[]], score 150, typed \" \"\r?\nclosed, a jump: score 250\r?\n$")
endif()

# The packages' tests draw on the tests' Vulkan too.
if(FBENGINE_FB_TESTS)
  foreach(package IN LISTS fbengine_packages)
    if(EXISTS "${package}/project.toml")
      cmake_path(GET package FILENAME name)
      set_property(TEST ${name}.package APPEND PROPERTY ENVIRONMENT ${fbengine_vulkan_environment})
    endif()
  endforeach()
endif()

# Texture's panics, each a case of tests/texture/panics run on its own, which has to panic saying
# what it does, on the tests' Vulkan.
if(FBENGINE_FB_TESTS)
  set(fbengine_texture_panics
      "wrapped-size=32768 by 16385 pixels of RGBA16F are 4295229440 bytes, not 262144"
      "huge-read=level 0's 2147483648 bytes are more than an array holds"
      "sprite-frame=the sprite's frames are 0 to 1, not 2"
      "canvas-freed=DrawRect on a Texture that was freed"
      "pixels-freed=Get on Pixels whose Texture was freed"
      "pixels-stopped=Set on Pixels whose Texture was freed"
      "pixels-drawn-over=the GPU changed the texture after its pixels came to the CPU, so they're put there again to be written"
      "lod-bias=a LOD bias is -16 to 16, not 17"
      "anisotropy=anisotropy is 1, for none, to 16, not 0.5"
      "screen-samples=the screen draws 1, 2, 4, or 8 samples a pixel, not 3")
  foreach(case IN LISTS fbengine_texture_panics)
    string(FIND "${case}" "=" split)
    string(SUBSTRING "${case}" 0 ${split} name)
    math(EXPR split "${split} + 1")
    string(SUBSTRING "${case}" ${split} -1 message)
    add_test(NAME texture.panics.${name}
             COMMAND "${FBENGINE_FB}" --color never run "${CMAKE_CURRENT_SOURCE_DIR}/texture/panics"
                     -- ${name})
    set_tests_properties(texture.panics.${name} PROPERTIES
                         PASS_REGULAR_EXPRESSION "panic: ${message}"
                         ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}")
  endforeach()
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

# text's atlas and layouts, as the canvas reads them, from its sources and core's, on HarfBuzz's
# test fonts.
file(GLOB fbengine_text_sources CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/native/fbengine/text/*.cpp")
add_executable(fbengine_text_tests text/text_tests.cpp ${fbengine_text_sources}
               "${PROJECT_SOURCE_DIR}/native/fbengine/core/core.cpp")
target_include_directories(fbengine_text_tests PRIVATE "${PROJECT_SOURCE_DIR}/native/fbengine")
target_compile_definitions(fbengine_text_tests PRIVATE FBE_BUILDING
                           FBE_VERSION="${PROJECT_VERSION}"
                           FBE_TEST_FONTS="${harfbuzz_SOURCE_DIR}/test/api/fonts"
                           FBE_OWN_TEST_FONTS="${CMAKE_CURRENT_SOURCE_DIR}/fonts"
                           FBE_SHAPE_TEST_FONTS="${harfbuzz_SOURCE_DIR}/test/shape/data/in-house/fonts")
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
# Its float arithmetic as written, never fused, as fbengine_native's is, so its corners and the
# GPU's it's checked against round alike on every target.
if(NOT MSVC)
  target_compile_options(fbengine_collide_tests PRIVATE -ffp-contract=off)
endif()
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
  # The IME's composition, set before the window opens, drawn at a field's cursor.
  add_test(NAME ui.composition
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/tests/ui" -- composition)
  set_tests_properties(ui.composition PROPERTIES
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;FBENGINE_OWN_TEST_FONTS=${CMAKE_CURRENT_SOURCE_DIR}/fonts;${fbengine_vulkan_environment}"
                       PASS_REGULAR_EXPRESSION "underlined True, cursor True, typed a\r?\nhalf size, underlined True" TIMEOUT 120)
  add_test(NAME ui.composition-moved
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/tests/ui" -- composition-moved)
  set_tests_properties(ui.composition-moved PROPERTIES
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       PASS_REGULAR_EXPRESSION "composing bb, then none" TIMEOUT 120)
  add_test(NAME ui.pop-clip-too-many
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/tests/ui" -- pop-clip-too-many)
  set_tests_properties(ui.pop-clip-too-many PROPERTIES
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${fbengine_vulkan_environment}"
                       PASS_REGULAR_EXPRESSION "Painter[.]PopClip: Node \"greedy\" ends a clip it didn't push" TIMEOUT 120)
endif()

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
               "${PROJECT_SOURCE_DIR}/native/fbengine/vfs/vfs.cpp"
               tools/pak_tests.cpp "${PROJECT_SOURCE_DIR}/native/fbengine_tools/cook/pak.cpp"
               "${PROJECT_SOURCE_DIR}/native/fbengine_tools/tools/atomic.cpp")
target_include_directories(fbengine_tools_tests PRIVATE
                           "${PROJECT_SOURCE_DIR}/native/fbengine_tools"
                           "${PROJECT_SOURCE_DIR}/native/fbengine")
# basisu's job pool, which libktx's encoder is patched in (cmake/assets.cmake).
target_include_directories(fbengine_tools_tests SYSTEM PRIVATE
                           "${ktx_SOURCE_DIR}/external/basisu/encoder")
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

# The job system's threads, started as the process ends, which mustn't hang its exit.
if(FBENGINE_FB_TESTS)
  add_test(NAME jobs.ends COMMAND "${FBENGINE_FB}" --color never run
           "${CMAKE_CURRENT_SOURCE_DIR}/jobs/ends")
  set_tests_properties(jobs.ends PROPERTIES TIMEOUT 180
                       PASS_REGULAR_EXPRESSION "
each ended
?
$")
endif()

# E8's done-when, a game whose build step is fbengine_tools', built each way, one at a time, as they
# share its build folder: its files as they are, one reloading as it changes, then cooked into its
# pak.
if(FBENGINE_FB_TESTS)
  # The cooker's threads, which can't hold up a whole run if they ever hang again.
  set_tests_properties(fbengine_tools.package PROPERTIES TIMEOUT 600)
  set_tests_properties(fbengine.package PROPERTIES TIMEOUT 1800)
  # A copy, whose path dependencies are the packages', and whose file a run can change.
  set(fbengine_game "${CMAKE_CURRENT_BINARY_DIR}/cooked_game")
  set(fbengine_game_source "${PROJECT_SOURCE_DIR}/examples/cooked_game")
  file(READ "${fbengine_game_source}/project.toml" fbengine_game_project)
  string(REPLACE "../../packages" "${PROJECT_SOURCE_DIR}/packages" fbengine_game_project
                 "${fbengine_game_project}")
  configure_file("${fbengine_game_source}/main.fb" "${fbengine_game}/main.fb" COPYONLY)
  file(COPY "${fbengine_game_source}/assets" DESTINATION "${fbengine_game}")
  file(CONFIGURE OUTPUT "${fbengine_game}/project.toml" CONTENT "${fbengine_game_project}" @ONLY)
  add_test(NAME examples.cooked_game.debug
           COMMAND "${FBENGINE_FB}" --color never run "${fbengine_game}" -- --change
                   "${fbengine_game}/assets/hero.png")
  set_tests_properties(examples.cooked_game.debug PROPERTIES RESOURCE_LOCK fbengine_game TIMEOUT 300
                       ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen" PASS_REGULAR_EXPRESSION
                       "\nlevel one\r?\nhero, as it is\r?\nhero reloaded, 8 wide\r?\n$")
  add_test(NAME examples.cooked_game.release
           COMMAND "${FBENGINE_FB}" --color never run --release "${fbengine_game}")
  set_tests_properties(examples.cooked_game.release PROPERTIES RESOURCE_LOCK fbengine_game
                       TIMEOUT 300 ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen"
                       PASS_REGULAR_EXPRESSION "\nlevel one\r?\nhero, cooked\r?\n$")
  # One without its assets/, whose step fails, saying so.
  set(fbengine_game_bare "${CMAKE_CURRENT_BINARY_DIR}/cooked_game_bare")
  configure_file("${fbengine_game_source}/main.fb" "${fbengine_game_bare}/main.fb" COPYONLY)
  file(CONFIGURE OUTPUT "${fbengine_game_bare}/project.toml" CONTENT "${fbengine_game_project}" @ONLY)
  add_test(NAME tools.game.bare COMMAND "${FBENGINE_FB}" --color never build "${fbengine_game_bare}")
  set_tests_properties(tools.game.bare PROPERTIES PASS_REGULAR_EXPRESSION
                       "assets isn't there: it holds what cooked_game has at res://")
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
  # What fbengine_audio panics for with fbengine's entities, each by a run of tests/audio/panics.
  foreach(case IN ITEMS "emitter-follows-freed;an Emitter can't follow a freed Pivot"
                        "listener-follows-freed;the Listener can't follow a freed Pivot")
    list(GET case 0 name)
    list(GET case 1 said)
    add_test(NAME audio.panics.${name}
             COMMAND "${FBENGINE_FB}" --color never run "${CMAKE_CURRENT_SOURCE_DIR}/audio/panics"
                     -- ${name})
    set_tests_properties(audio.panics.${name} PROPERTIES TIMEOUT 60
                         PASS_REGULAR_EXPRESSION "panic: ${said}")
  endforeach()
endif()

# fbengine_native's frame, each test in a process of its own, as a program's first frames are.
add_executable(fbengine_core_tests core/frame_tests.cpp)
target_link_libraries(fbengine_core_tests PRIVATE fbengine_native Catch2::Catch2WithMain)
fbengine_set_warnings(fbengine_core_tests)
catch_discover_tests(fbengine_core_tests TEST_PREFIX "core."
                     DL_PATHS "${fbengine_test_dll_paths};$<TARGET_FILE_DIR:fbengine_native>")

# E7's animation: sampling, apart from the DLL, and the clip, player, and skin areas through
# fbengine's C API, with no window or GPU.
add_executable(fbengine_anim_tests anim/sample_tests.cpp anim/clip_tests.cpp anim/player_tests.cpp
               anim/skin_tests.cpp anim/model_tests.cpp
               "${PROJECT_SOURCE_DIR}/native/fbengine/clip/sample.cpp")
target_link_libraries(fbengine_anim_tests PRIVATE fbengine_native Catch2::Catch2WithMain)
fbengine_set_warnings(fbengine_anim_tests)
catch_discover_tests(fbengine_anim_tests TEST_PREFIX "anim."
                     DL_PATHS "${fbengine_test_dll_paths}" "${PROJECT_BINARY_DIR}/native/fbengine")

# Animation's panics, each a case of tests/anim/panics run on its own, which has to panic saying
# what it does.
if(FBENGINE_FB_TESTS)
  set(fbengine_anim_panics
      "frame-rate=Clip: a clip's frames a second are 0 or more, not -1"
      "unpaired=AddPositions on a Clip: a key's time for each value, not 2 for 1"
      "untangented=AddRotations on a Clip: a CUBIC channel's keys have tangents"
      "tangents-unpaired=AddScales on a Clip: a key's time and tangents for each value"
      "times-falling=AddPositions on a Clip: each key's time is after the last: 1 isn't after 1"
      "moved-twice=AddScales on a Clip: the clip moves the scale of \"a\" already"
      "clip-freed=Length on a Clip that was freed"
      "player-of-freed=Player on a Pivot that was freed"
      "player-entity-freed=SetTime on a Player whose entity was freed"
      "time-nan=SetTime on a Player: a clip's time is a number of seconds, not nan"
      "update-negative=Update on a World: a world is moved on by 0 seconds or more, not -1"
      "palette-unskinned=Palette on a Mesh skinned by nothing"
      "joint-twice=AddJoint on a Skin: the skin has a joint \"a\" already"
      "joint-out-of-range=JointName\\(0\\) on a Skin with 0 joints"
      "layer-out-of-range=Layer\\(16\\) on a Player: its layers are 0 to 15"
      "crossfade-negative=Crossfade on a Player: a fade takes 0 seconds or more, not -1"
      "weight-outside=SetWeight on a Player: a layer's weight is from 0 to 1, not 2"
      "event-out-of-range=EventName\\(0\\) on a Clip with 0 events"
      "slice-backward=Slice on a Clip: a clip's part is from 0 seconds or more to then or later, not 2 to 1")
  foreach(case IN LISTS fbengine_anim_panics)
    string(FIND "${case}" "=" split)
    string(SUBSTRING "${case}" 0 ${split} name)
    math(EXPR split "${split} + 1")
    string(SUBSTRING "${case}" ${split} -1 message)
    add_test(NAME anim.panics.${name}
             COMMAND "${FBENGINE_FB}" --color never run "${CMAKE_CURRENT_SOURCE_DIR}/anim/panics"
                     -- ${name})
    set_tests_properties(anim.panics.${name} PROPERTIES TIMEOUT 60
                         PASS_REGULAR_EXPRESSION "panic: ${message}")
  endforeach()
endif()
