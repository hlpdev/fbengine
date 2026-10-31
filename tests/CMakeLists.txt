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
      set_tests_properties(${name}.package PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen")
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
      "replace-overflows|panic: Action[.]Replace: a binding's scale is a finite number, not Inf")
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
endif()

# The scene, through fbengine's C API, with no window or GPU.
add_executable(fbengine_scene_tests scene/world_tests.cpp scene/transform_tests.cpp
               scene/camera_tests.cpp scene/light_tests.cpp scene/geometry_tests.cpp
               scene/material_tests.cpp scene/bvh_tests.cpp scene/collision_tests.cpp
               scene/handles_tests.cpp scene/pick_tests.cpp
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
      "camera-pick-freed=Pick on a Camera that was freed")
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

# fbengine's app area apart from its DLL: what it shows nothing for, and what a link opens, on
# Windows read back through the shell; it opens nothing.
add_executable(fbengine_app_tests app/app_tests.cpp "${PROJECT_SOURCE_DIR}/native/fbengine/app/link.cpp")
target_include_directories(fbengine_app_tests PRIVATE "${PROJECT_SOURCE_DIR}/native/fbengine")
target_link_libraries(fbengine_app_tests PRIVATE SDL3::SDL3-static Catch2::Catch2WithMain)
if(WIN32)
  target_link_libraries(fbengine_app_tests PRIVATE shlwapi)
endif()
fbengine_set_warnings(fbengine_app_tests)
catch_discover_tests(fbengine_app_tests TEST_PREFIX "app."
                     DL_PATHS "${fbengine_test_dll_paths}")
