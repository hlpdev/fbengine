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

# Compiles a target's .slang files to SPIR-V as it builds and embeds them in it: `header`, which the
# target includes by its name, declares a fbe::gpu::shader_t for each, named as its file is, in
# `namespace`. Every entry point a file has is in its SPIR-V, by its own name; matrices are
# column-major, as std.math's are; and a warning is an error.
function(fbengine_add_shaders target namespace header)
  set(folder "${CMAKE_CURRENT_BINARY_DIR}/${target}_shaders")
  set(shaders "")
  set(compiled "")
  foreach(source IN LISTS ARGN)
    cmake_path(ABSOLUTE_PATH source NORMALIZE)
    cmake_path(GET source STEM name)
    set(spirv "${folder}/${name}.spv")
    add_custom_command(
      OUTPUT "${spirv}"
      COMMAND "${FBENGINE_SLANGC}" "${source}" -target spirv -profile spirv_1_5
              -fvk-use-entrypoint-name -matrix-layout-column-major -O2 -warnings-as-errors all
              -o "${spirv}" -depfile "${spirv}.d"
      DEPENDS "${source}"
      DEPFILE "${spirv}.d"
      COMMENT "Compiling ${name}.slang to SPIR-V"
      VERBATIM)
    list(APPEND shaders "${name}=${spirv}")
    list(APPEND compiled "${spirv}")
  endforeach()
  # A list's semicolons would split the command's argument, so the script takes bars.
  string(REPLACE ";" "|" shaders "${shaders}")
  cmake_path(GET header STEM stem)
  add_custom_command(
    OUTPUT "${folder}/${header}" "${folder}/${stem}.cpp"
    COMMAND "${CMAKE_COMMAND}" "-DHEADER=${folder}/${header}" "-DSOURCE=${folder}/${stem}.cpp"
            "-DNAMESPACE=${namespace}" "-DSHADERS=${shaders}"
            -P "${PROJECT_SOURCE_DIR}/cmake/embed_shaders.cmake"
    DEPENDS ${compiled} "${PROJECT_SOURCE_DIR}/cmake/embed_shaders.cmake"
    COMMENT "Embedding ${target}'s shaders"
    VERBATIM)
  target_sources(${target} PRIVATE "${folder}/${stem}.cpp")
  target_include_directories(${target} PRIVATE "${folder}")
endfunction()
