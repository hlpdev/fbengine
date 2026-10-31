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
# fetched; on Linux the system's (apt's mesa-vulkan-drivers); on macOS it's MoltenVK, the
# system's. FBENGINE_VULKAN_DRIVER names another.
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
  if(fbengine_lavapipes)
    list(GET fbengine_lavapipes 0 fbengine_vulkan_driver)
  endif()
endif()
# Windows' loader takes only backslashes.
set(fbengine_vulkan_environment "")
if(NOT fbengine_vulkan_driver STREQUAL "")
  cmake_path(NATIVE_PATH fbengine_vulkan_driver fbengine_vulkan_driver)
  set(fbengine_vulkan_environment "VK_DRIVER_FILES=${fbengine_vulkan_driver}")
endif()

# Vulkan with no window, on the system's loader, which volk loads, and the tests' driver; and the
# GPU through fbengine_native's C API, whose DLL is copied beside them on Windows.
add_executable(fbengine_gpu_tests gpu/vulkan_tests.cpp gpu/present_tests.cpp)
target_link_libraries(fbengine_gpu_tests PRIVATE volk Vulkan::Headers Catch2::Catch2WithMain
                      fbengine_native)
fbengine_set_warnings(fbengine_gpu_tests)
if(WIN32)
  add_custom_command(TARGET fbengine_gpu_tests POST_BUILD
                     COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                             "$<TARGET_FILE:fbengine_native>" "$<TARGET_FILE_DIR:fbengine_gpu_tests>"
                     VERBATIM)
endif()
catch_discover_tests(fbengine_gpu_tests TEST_PREFIX "gpu."
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
