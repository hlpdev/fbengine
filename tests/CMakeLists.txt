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
