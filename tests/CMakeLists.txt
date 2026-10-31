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
endif()

# The scene, through fbengine's C API, with no window or GPU.
add_executable(fbengine_scene_tests scene/world_tests.cpp scene/transform_tests.cpp
               scene/camera_tests.cpp scene/light_tests.cpp scene/geometry_tests.cpp
               scene/material_tests.cpp scene/bvh_tests.cpp
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
      "free-main=the main World stays"
      "own-parent=an entity can't be its own parent"
      "under-itself=an entity can't go under what's under it"
      "child-out-of-range=an entity's children are at 0 to 1, not 2"
      "data-freed=Data on a Pivot that was freed"
      "poses-count=SetPoses takes a pose for each entity, not 0 for 1"
      "poses-freed=Poses on a Pivot that was freed"
      "into-nothing=the entity to transform into is scaled to nothing on an axis"
      "turn-freed=Turn on a Pivot that was freed"
      "field-of-view=a field of view is between 0 and Pi, not 0"
      "range=a camera's range is 0 < near < far, finite, not 5 to 1"
      "viewport=a viewport has a finite place and a size above 0"
      "no-window=the camera's viewport has no size: open the window, or give it a viewport"
      "camera-freed=SetOrder on a Camera that was freed"
      "intensity=a light's intensity is 0 or more, not -1"
      "light-range=a light's range is above 0, not 0"
      "cone=a spot light's cone is 0 <= inner <= outer <= Pi / 2"
      "fog-range=a fog's range is 0 <= start < end, finite, not 5 to 5"
      "fog-density=a fog's density is 0 or more"
      "light-freed=SetColor on a Light that was freed"
      "segments=a shape has 3 to 100 segments, not 2"
      "triangle-index=the surface's vertices are 0 to 2, not 5"
      "triangle-count=triangles take indices three at a time, not 4"
      "surface-index=the geometry's surfaces are 0 to 0, not 1"
      "vertex-index=the surface's vertices are 0 to 2, not 3 to 3"
      "uv-set=a vertex's UV sets are 0 and 1, not 2"
      "geometry-freed=AddSurface on a Geometry that was freed"
      "mesh-freed-geometry=SetGeometry on a Geometry that was freed"
      "shininess=a material's shininess is 0 to 1, not 2"
      "alpha-cutoff=an alpha cutoff is 0 to 1, not -1"
      "layer=a material's layers are 0 to 7, not 8"
      "layer-uv-set=a layer's UV set is 0 or 1, not 2"
      "material-freed=SetOrder on a Material that was freed"
      "surface-freed-material=SetMaterial on a Material that was freed")
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
