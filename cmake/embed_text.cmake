# Embeds text files in C++, as `cmake -P` runs it: a header declaring fbe::tools::shader_sources, and
# the source that has their bytes, as C strings of hex escapes split so no literal's too long.
#
#   cmake -DHEADER=<.hpp> -DSOURCE=<.cpp> -DFILES=<path|..> -P embed_text.cmake

cmake_path(GET HEADER FILENAME header_name)
string(REPLACE "|" ";" FILES "${FILES}")
set(arrays "")
set(entries "")
set(index 0)
foreach(path IN LISTS FILES)
  cmake_path(GET path FILENAME name)
  file(READ "${path}" bytes HEX)
  string(REGEX REPLACE "(..)" "0x\\1, " listed "${bytes}")
  string(APPEND arrays "constexpr char text_${index}[] = { ${listed}0 };\n")
  string(APPEND entries "    { \"${name}\", text_${index} },\n")
  math(EXPR index "${index} + 1")
endforeach()
file(CONFIGURE OUTPUT "${HEADER}" CONTENT "// Made by cmake/embed_text.cmake.

#pragma once

#include <span>

namespace fbe::tools {

struct shader_source_t {
    const char* name;
    const char* text;
};

/// The engine's shaders' sources, as they were when it was built.
std::span<const shader_source_t> shader_sources();

} // namespace fbe::tools
" @ONLY)
file(CONFIGURE OUTPUT "${SOURCE}" CONTENT "// Made by cmake/embed_text.cmake: the shaders' sources.

#include \"${header_name}\"

namespace {

// NOLINTBEGIN
${arrays}// NOLINTEND

const fbe::tools::shader_source_t sources[] = {
${entries}};

} // namespace

namespace fbe::tools {

std::span<const shader_source_t> shader_sources() {
    return sources;
}

} // namespace fbe::tools
" @ONLY)
