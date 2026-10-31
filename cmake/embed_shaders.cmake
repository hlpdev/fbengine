# Embeds SPIR-V in C++, as `cmake -P` runs it for fbengine_add_shaders: a header declaring a
# fbe::gpu::shader_t for each shader, in NAMESPACE, and the source that has their words.
#
#   cmake -DHEADER=<.hpp> -DSOURCE=<.cpp> -DNAMESPACE=<ns> -DSHADERS=<name=path|...> -P embed_shaders.cmake

cmake_path(GET HEADER FILENAME header_name)
string(REPLACE "|" ";" SHADERS "${SHADERS}")
set(declarations "")
set(definitions "")
set(words "")
foreach(shader IN LISTS SHADERS)
  string(FIND "${shader}" "=" split)
  string(SUBSTRING "${shader}" 0 ${split} name)
  math(EXPR split "${split} + 1")
  string(SUBSTRING "${shader}" ${split} -1 path)

  file(READ "${path}" bytes HEX)
  string(LENGTH "${bytes}" length)
  math(EXPR remainder "${length} % 8")
  if(length EQUAL 0 OR NOT remainder EQUAL 0)
    message(FATAL_ERROR "${path} isn't SPIR-V: it isn't whole 32-bit words")
  endif()
  # SPIR-V's words are little-endian in the file, and C++'s literals big-endian on the page.
  string(REGEX REPLACE "(..)(..)(..)(..)" "0x\\4\\3\\2\\1, " code "${bytes}")
  string(APPEND words "constexpr std::uint32_t ${name}_code[] = { ${code}};\n")
  string(APPEND declarations "extern const fbe::gpu::shader_t ${name};\n")
  string(APPEND definitions "const fbe::gpu::shader_t ${name}{ \"${name}\", ${name}_code };\n")
endforeach()

set(header_text "#pragma once

// Made by cmake/embed_shaders.cmake: the shaders fbengine_add_shaders compiled.

#include \"gpu/shader.hpp\"

namespace ${NAMESPACE} {

${declarations}
} // namespace ${NAMESPACE}
")
set(source_text "// Made by cmake/embed_shaders.cmake: the shaders' SPIR-V.

#include \"${header_name}\"

#include <cstdint>

namespace {

// NOLINTBEGIN
${words}// NOLINTEND

} // namespace

namespace ${NAMESPACE} {

${definitions}
} // namespace ${NAMESPACE}
")
# Written only when they change, so what includes them isn't built again for nothing.
foreach(kind IN ITEMS header source)
  set(file "${${kind}_text}")
  string(TOUPPER "${kind}" variable)
  set(old "")
  if(EXISTS "${${variable}}")
    file(READ "${${variable}}" old)
  endif()
  if(NOT old STREQUAL file)
    file(WRITE "${${variable}}" "${file}")
  endif()
endforeach()
