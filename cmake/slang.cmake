# Slang's compiler, slangc, pinned to a release, for the machine building: shaders are compiled to
# SPIR-V as the engine builds and embedded in what's built (fbengine_add_shaders), so nothing at run
# time compiles them.
set(fbengine_slang_version 2026.20)
set(fbengine_slang_url
    "https://github.com/shader-slang/slang/releases/download/v${fbengine_slang_version}")
if(CMAKE_HOST_WIN32)
  set(fbengine_slang_asset windows-x86_64.zip)
  set(fbengine_slang_hash e6dd32bbddf1720ebb4e679d55e9635ce792865a5bd84f3d2cd9af28647c0174)
elseif(CMAKE_HOST_APPLE AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
  set(fbengine_slang_asset macos-aarch64.tar.gz)
  set(fbengine_slang_hash 07fa8833fdbb343c3674c84fdd885910f495f3daaaa0c732b32980151e8b58c4)
elseif(CMAKE_HOST_APPLE)
  set(fbengine_slang_asset macos-x86_64.tar.gz)
  set(fbengine_slang_hash b4f01f615dc625f60c593d7d0a4bbd38e05dfa2a34bfd3973017c309d866feaa)
elseif(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
  set(fbengine_slang_asset linux-aarch64-glibc-2.28.tar.gz)
  set(fbengine_slang_hash 9959d741713162efdbd863ff6d32400432b6e103eae3a9ee0afbdba6a00c6a9e)
else()
  set(fbengine_slang_asset linux-x86_64-glibc-2.28.tar.gz)
  set(fbengine_slang_hash 84ebd777968caceea26b21bfd014341078be6047be6c5a963a69f984f896cebd)
endif()
FetchContent_Declare(
  slang
  URL "${fbengine_slang_url}/slang-${fbengine_slang_version}-${fbengine_slang_asset}"
  URL_HASH SHA256=${fbengine_slang_hash}
  SOURCE_SUBDIR none
)
FetchContent_MakeAvailable(slang)
find_program(FBENGINE_SLANGC slangc PATHS "${slang_SOURCE_DIR}/bin" NO_DEFAULT_PATH REQUIRED
             DOC "Slang's compiler, the pinned release's")

# What a game's shader is compiled with, as one hash: Slang's release, and the sources it's compiled
# against, whose layouts can change with no new version of the engine. Line ends are left out, which
# a checkout changes, so a shader cooked on one machine loads on another.
set(fbengine_shader_sources
    "${PROJECT_SOURCE_DIR}/native/fbengine/shaders/scene.slang"
    "${PROJECT_SOURCE_DIR}/native/fbengine/shaders/own_shader.slang"
    "${PROJECT_SOURCE_DIR}/native/fbengine/shaders/bindless.slang"
    "${PROJECT_SOURCE_DIR}/native/fbengine/shaders/pbr.slang")
set(fbengine_shader_stamped "${fbengine_slang_version}")
foreach(source IN LISTS fbengine_shader_sources)
  file(READ "${source}" text)
  string(REPLACE "\r\n" "\n" text "${text}")
  string(APPEND fbengine_shader_stamped "${text}")
  set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
endforeach()
string(SHA256 fbengine_shader_stamp "${fbengine_shader_stamped}")

# Compiles .slang files to SPIR-V as the engine builds and embeds them in each of TARGETS, which
# share the one compile: HEADER, which a target includes by its name, declares a fbe::gpu::shader_t
# for each file, named as the file is, in NAMESPACE.
#
#   fbengine_add_shaders(TARGETS <target>... NAMESPACE <namespace> HEADER <name.hpp>
#                        SHADERS <file.slang>... [INCLUDES <folder>...])
#
# Every entry point a file has is in its SPIR-V, by its own name; matrices are column-major, as
# std.math's are; a file can import the engine's modules, as bindless.slang; and a warning is an
# error, but for Slang's saying it widened the profile, which it does for any texture sampled and
# which emits nothing the SPIR-V doesn't use. A new slangc, or a new release of it, compiles them
# all again.
function(fbengine_add_shaders)
  cmake_parse_arguments(PARSE_ARGV 0 arg "" "NAMESPACE;HEADER" "TARGETS;SHADERS;INCLUDES")
  set(includes "")
  foreach(include IN LISTS arg_INCLUDES)
    cmake_path(ABSOLUTE_PATH include NORMALIZE)
    list(APPEND includes -I "${include}")
  endforeach()
  cmake_path(GET arg_HEADER STEM stem)
  set(folder "${CMAKE_CURRENT_BINARY_DIR}/${stem}")
  # slangc makes no folder for what it writes, and only Ninja makes one for a command's output.
  file(MAKE_DIRECTORY "${folder}")
  set(shaders "")
  set(compiled "")
  foreach(source IN LISTS arg_SHADERS)
    cmake_path(ABSOLUTE_PATH source NORMALIZE)
    cmake_path(GET source STEM name)
    set(spirv "${folder}/${name}.spv")
    add_custom_command(
      OUTPUT "${spirv}"
      COMMAND "${FBENGINE_SLANGC}" "${source}" -target spirv -profile spirv_1_5
              -fvk-use-entrypoint-name -matrix-layout-column-major -O2 -warnings-as-errors all
              -warnings-disable 41012 -I "${PROJECT_SOURCE_DIR}/native/fbengine/shaders" ${includes}
              "-DFBENGINE_SLANG=${fbengine_slang_version}" -o "${spirv}" -depfile "${spirv}.d"
      DEPENDS "${source}" "${FBENGINE_SLANGC}"
      DEPFILE "${spirv}.d"
      COMMENT "Compiling ${name}.slang to SPIR-V"
      VERBATIM)
    list(APPEND shaders "${name}=${spirv}")
    list(APPEND compiled "${spirv}")
  endforeach()
  # A list's semicolons would split the command's argument, so the script takes bars.
  string(REPLACE ";" "|" shaders "${shaders}")
  add_custom_command(
    OUTPUT "${folder}/${arg_HEADER}" "${folder}/${stem}.cpp"
    COMMAND "${CMAKE_COMMAND}" "-DHEADER=${folder}/${arg_HEADER}" "-DSOURCE=${folder}/${stem}.cpp"
            "-DNAMESPACE=${arg_NAMESPACE}" "-DSHADERS=${shaders}"
            -P "${PROJECT_SOURCE_DIR}/cmake/embed_shaders.cmake"
    DEPENDS ${compiled} "${PROJECT_SOURCE_DIR}/cmake/embed_shaders.cmake"
    COMMENT "Embedding ${arg_HEADER}'s shaders"
    VERBATIM)
  foreach(target IN LISTS arg_TARGETS)
    target_sources(${target} PRIVATE "${folder}/${stem}.cpp")
    target_include_directories(${target} PRIVATE "${folder}")
  endforeach()
endfunction()
