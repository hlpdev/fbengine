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
  add_test(NAME examples.door_and_trigger
           COMMAND "${FBENGINE_FB}" --color never run
                   "${PROJECT_SOURCE_DIR}/examples/door_and_trigger")
  set_tests_properties(examples.door_and_trigger PROPERTIES PASS_REGULAR_EXPRESSION
                       "\nthe door swung open: True\r?\nthe player walked into the room: True, and is in it: True\r?\n$")
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
