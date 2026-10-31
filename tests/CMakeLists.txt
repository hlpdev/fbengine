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
               golden/golden.cpp golden/golden_tests.cpp golden/stb.cpp)
target_link_libraries(fbengine_gpu_tests PRIVATE volk Vulkan::Headers Catch2::Catch2WithMain
                      fbengine_internal)
target_include_directories(fbengine_gpu_tests SYSTEM PRIVATE "${stb_SOURCE_DIR}")
target_include_directories(fbengine_gpu_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(fbengine_gpu_tests PRIVATE
                           FBENGINE_GOLDEN_IMAGES="${CMAKE_CURRENT_SOURCE_DIR}/golden"
                           FBENGINE_GOLDEN_FAILURES="${PROJECT_BINARY_DIR}/golden-failures"
                           FBENGINE_GOLDEN_UPDATES="${PROJECT_BINARY_DIR}/golden-updates"
                           FBENGINE_TEST_SCRATCH="${PROJECT_BINARY_DIR}/test-scratch")
fbengine_set_warnings(fbengine_gpu_tests)
fbengine_add_shaders(TARGETS fbengine_gpu_tests NAMESPACE fbe::test_shaders
                     HEADER test_shaders.hpp
                     SHADERS gpu/shaders/triangle.slang gpu/shaders/textured.slang
                             gpu/shaders/shape.slang)
catch_discover_tests(fbengine_gpu_tests TEST_PREFIX "gpu." ADD_TAGS_AS_LABELS
                     DL_PATHS "${fbengine_test_dll_paths}"
                     PROPERTIES ENVIRONMENT "${fbengine_vulkan_environment}")

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
      set_tests_properties(${name}.package PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen")
    endif()
  endforeach()

  # fb's own lines, building the program, come before what it prints.
  add_test(NAME examples.hello
           COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/hello")
  set_tests_properties(examples.hello PROPERTIES
                       PASS_REGULAR_EXPRESSION "Running hello[^\n]*\nfbengine ${PROJECT_VERSION}\r?\n$")
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
