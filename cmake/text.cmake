# Text's libraries, built into fbengine_native statically: FreeType rasterizes fonts' glyphs,
# HarfBuzz shapes text into them, and SheenBidi splits text into runs by direction, which
# HarfBuzz shapes one at a time. FreeType's optional parts are left out: zlib, bzip2, and Brotli
# for compressed fonts, PNG for bitmap emoji, and HarfBuzz for its autohinter.
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)

FetchContent_Declare(
  freetype
  GIT_REPOSITORY https://github.com/freetype/freetype.git
  GIT_TAG VER-2-15-0
  GIT_SHALLOW TRUE
  SYSTEM
)
# HarfBuzz and SheenBidi are built here from their sources, not their own builds, which a
# SOURCE_SUBDIR with no CMakeLists.txt leaves out.
FetchContent_Declare(
  harfbuzz
  GIT_REPOSITORY https://github.com/harfbuzz/harfbuzz.git
  GIT_TAG 14.6.0
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR none
  SYSTEM
)
FetchContent_Declare(
  sheenbidi
  GIT_REPOSITORY https://github.com/Tehreer/SheenBidi.git
  GIT_TAG v3.0.0
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR none
  SYSTEM
)
FetchContent_MakeAvailable(freetype harfbuzz sheenbidi)

# Built into a shared library, their code is position-independent too. FreeType registers only
# the modules TrueType and OpenType fonts need (cmake/freetype/fbengine_ftmodule.h), so the rest
# aren't linked.
set_target_properties(freetype PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(freetype PRIVATE "${CMAKE_CURRENT_LIST_DIR}/freetype")
# C's only: Windows' resource compiler can't take the angle brackets.
target_compile_definitions(
  freetype PRIVATE "$<$<COMPILE_LANGUAGE:C>:FT_CONFIG_MODULES_H=<fbengine_ftmodule.h$<ANGLE-R>>")

# HarfBuzz from its one-file build, with its own OpenType functions, so it needs nothing else. Its
# one object has more sections than COFF's 65,535, which MinGW's and MSVC's big objects take.
add_library(fbengine_harfbuzz STATIC "${harfbuzz_SOURCE_DIR}/src/harfbuzz.cc")
target_include_directories(fbengine_harfbuzz SYSTEM PUBLIC "${harfbuzz_SOURCE_DIR}/src")
set_target_properties(fbengine_harfbuzz PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MINGW)
  target_compile_options(fbengine_harfbuzz PRIVATE -Wa,-mbig-obj)
elseif(MSVC)
  target_compile_options(fbengine_harfbuzz PRIVATE /bigobj)
endif()

# SheenBidi from its one-file build.
add_library(fbengine_sheenbidi STATIC "${sheenbidi_SOURCE_DIR}/Source/SheenBidi.c")
target_compile_definitions(fbengine_sheenbidi PRIVATE SB_CONFIG_UNITY)
target_include_directories(fbengine_sheenbidi SYSTEM PUBLIC "${sheenbidi_SOURCE_DIR}/Headers"
                           PRIVATE "${sheenbidi_SOURCE_DIR}/Source")
set_target_properties(fbengine_sheenbidi PROPERTIES POSITION_INDEPENDENT_CODE ON)
