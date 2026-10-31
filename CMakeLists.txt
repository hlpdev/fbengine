cmake_minimum_required(VERSION 3.28)

# fbengine's version is its packages', which are released together: the fbengine package's
# project.toml says it, and the others' say the same.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/packages/fbengine/project.toml" fbengine_version_line
     REGEX "^version = \"[0-9]+\\.[0-9]+\\.[0-9]+\"$" LIMIT_COUNT 1)
if(NOT fbengine_version_line MATCHES "\"([0-9]+\\.[0-9]+\\.[0-9]+)\"")
  message(FATAL_ERROR "packages/fbengine/project.toml has no version like \"0.1.0\"")
endif()
project(fbengine VERSION ${CMAKE_MATCH_1} LANGUAGES C CXX)
# A new version there configures again, so the DLLs say it.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/packages/fbengine/project.toml")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# No C++ modules here; scanning adds GCC-only flags that break clang-tidy and clangd.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(FBENGINE_BUILD_TESTS "Build the test suite" ON)

# MSVC otherwise reads source files in the ANSI code page.
if(MSVC)
  add_compile_options(/utf-8)
endif()

include(FetchContent)
include(cmake/warnings.cmake)
include(cmake/native.cmake)
include(cmake/sdl.cmake)

add_subdirectory(native/fbengine)

if(FBENGINE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
