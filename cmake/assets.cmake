# Builds `patched` in `target` in place of its source `relative` in `folder`, with that source's
# options, as what's patched is only part of a project that's fetched.
function(fbengine_replace_source target folder relative patched)
  set(original "${folder}/${relative}")
  get_target_property(sources ${target} SOURCES)
  set(replaced "")
  set(found FALSE)
  foreach(source IN LISTS sources)
    if(source STREQUAL "${original}" OR source STREQUAL "${relative}")
      list(APPEND replaced "${patched}")
      set(found TRUE)
    else()
      list(APPEND replaced "${source}")
    endif()
  endforeach()
  if(NOT found)
    message(FATAL_ERROR "${target} doesn't build ${relative}, which a patch replaces")
  endif()
  set_property(TARGET ${target} PROPERTY SOURCES "${replaced}")
  get_source_file_property(options "${original}" DIRECTORY "${folder}" COMPILE_OPTIONS)
  if(options)
    set_source_files_properties("${patched}" TARGET_DIRECTORY ${target} PROPERTIES COMPILE_OPTIONS
                                "${options}")
  endif()
endfunction()

# The loaders' libraries, built into fbengine_native statically: stb_image for PNG and JPEG,
# KTX-Software's libktx for KTX2 and its Basis Universal transcoder, and fastgltf for glTF. Each is
# left out of the build until something links it: the DLL links libktx's reader, and the tests its
# writer and encoder, which make the KTX2 files they read.

# stb has no releases, so it's pinned to a commit; it's headers, and so is its target here.
FetchContent_Declare(
  stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG 003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5
  SYSTEM
  EXCLUDE_FROM_ALL
)

# KTX1 stays in, since libktx doesn't link without it, though only KTX2s are read.
set(KTX_FEATURE_KTX1 ON CACHE BOOL "" FORCE)
set(KTX_FEATURE_TESTS OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_TOOLS OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_DOC OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_JNI OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_PY OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_GL_UPLOAD OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_VK_UPLOAD OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_ETC_UNPACK OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_LOADTEST_APPS "" CACHE STRING "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
# The ASTC encoder in libktx's writer, the cooker's, for SSE4.1, as the engine's other code is
# for x86-64's SSE4 at most, not AVX2.
set(ASTCENC_ISA_SSE41 ON CACHE BOOL "" FORCE)
FetchContent_Declare(
  KTX
  GIT_REPOSITORY https://github.com/KhronosGroup/KTX-Software.git
  GIT_TAG v4.4.2
  GIT_SHALLOW TRUE
  SYSTEM
  EXCLUDE_FROM_ALL
)

# simdjson, which fastgltf uses one of installed, as Homebrew's on macOS, or downloads. It gives
# what links it the headers of the copy it downloads, but links an installed one privately; the
# engine's glTF decoder reads extras through simdjson, as fastgltf hands them over, so an installed
# one is found here, where the engine's targets see it, and given them through fastgltf too.
find_package(simdjson CONFIG QUIET)
set(FASTGLTF_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  fastgltf
  GIT_REPOSITORY https://github.com/spnda/fastgltf.git
  GIT_TAG v0.9.1
  GIT_SHALLOW TRUE
  SYSTEM
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(stb KTX fastgltf)
if(TARGET simdjson::simdjson)
  target_link_libraries(fastgltf INTERFACE simdjson::simdjson)
endif()


# stb_image is patched where a PNG whose first IDAT is empty has stbi__getn copy none of it from
# a null buffer, which memcpy doesn't allow, and UBSan reports: reading nothing, it gives that it
# did before it copies. The patched copy comes before stb's own folder, and the patch's text has to
# be there, once, or configuring fails.
file(READ "${stb_SOURCE_DIR}/stb_image.h" fbengine_stb_image)
set(fbengine_stb_image_from
    "static int stbi__getn(stbi__context *s, stbi_uc *buffer, int n)\n{\n   if (s->io.read) {")
set(fbengine_stb_image_to
    "static int stbi__getn(stbi__context *s, stbi_uc *buffer, int n)\n{\n   if (n == 0) return 1;\n   if (s->io.read) {")
string(FIND "${fbengine_stb_image}" "${fbengine_stb_image_from}" fbengine_first)
string(FIND "${fbengine_stb_image}" "${fbengine_stb_image_from}" fbengine_last REVERSE)
if(fbengine_first EQUAL -1 OR NOT fbengine_first EQUAL fbengine_last)
  message(FATAL_ERROR "stb_image.h doesn't have, once, what its patch expects")
endif()
string(REPLACE "${fbengine_stb_image_from}" "${fbengine_stb_image_to}" fbengine_stb_image
               "${fbengine_stb_image}")
set(fbengine_stb_patched "${CMAKE_BINARY_DIR}/stb-patched")
file(WRITE "${fbengine_stb_patched}/stb_image.h.in" "${fbengine_stb_image}")
configure_file("${fbengine_stb_patched}/stb_image.h.in" "${fbengine_stb_patched}/stb_image.h"
               COPYONLY)
add_library(fbengine_stb INTERFACE)
target_include_directories(fbengine_stb SYSTEM INTERFACE "${fbengine_stb_patched}"
                           "${stb_SOURCE_DIR}")

# Built into a shared library, their code is position-independent too.
set_target_properties(ktx_read ktx fastgltf PROPERTIES POSITION_INDEPENDENT_CODE ON)

# MinGW's off_t, which libktx's offsets are, is 32-bit unless asked for 64, which libktx's debug
# checks and its files over 2 GB need; what includes ktx.h sees the same.
if(MINGW)
  target_compile_definitions(ktx_read PUBLIC _FILE_OFFSET_BITS=64)
  target_compile_definitions(ktx PUBLIC _FILE_OFFSET_BITS=64)
endif()

# fastgltf's asserts look at what a hostile glTF breaks before the engine can check it, and end the
# process in a debug build; its own code is built as a release's is, and the engine checks what they
# would. Two of its headers are patched, where the parse itself reads past a string that's too
# short, before the engine sees the data: a data URI's base64 of 0 to 3 characters, whose padding
# getPadding looks for before its start, and an accessor's type too short to be one, which
# getAccessorType reads past. The patched copies come before its own headers in its own code, which
# alone calls them, and each patch's text has to be there, once, or configuring fails. In a debug
# build, libstdc++'s own checks are on in it, so what reads out of range ends the tests.
target_compile_definitions(fastgltf PRIVATE NDEBUG $<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>)
set(fbengine_fastgltf_patches
    "base64.hpp"
    "\t\tassert(string.size() >= 4 && string.size() % 4 == 0)\;\n\t\tconst auto size = string.size()\;"
    "\t\tif (string.size() < 4 || string.size() % 4 != 0)\n\t\t\treturn 0\;\n\t\tconst auto size = string.size()\;"
    "types.hpp"
    "\t\tassert(!accessorTypeName.empty())\;\n\t\tswitch (accessorTypeName[0]) {"
    "\t\tif (accessorTypeName.size() < 4)\n\t\t\treturn AccessorType::Invalid\;\n\t\tswitch (accessorTypeName[0]) {")
set(fbengine_fastgltf_patched "${CMAKE_BINARY_DIR}/fastgltf-patched")
foreach(fbengine_patch RANGE 0 5 3)
  math(EXPR fbengine_patch_from "${fbengine_patch} + 1")
  math(EXPR fbengine_patch_to "${fbengine_patch} + 2")
  list(GET fbengine_fastgltf_patches ${fbengine_patch} fbengine_file)
  list(GET fbengine_fastgltf_patches ${fbengine_patch_from} fbengine_from)
  list(GET fbengine_fastgltf_patches ${fbengine_patch_to} fbengine_to)
  file(READ "${fastgltf_SOURCE_DIR}/include/fastgltf/${fbengine_file}" fbengine_text)
  string(FIND "${fbengine_text}" "${fbengine_from}" fbengine_first)
  string(FIND "${fbengine_text}" "${fbengine_from}" fbengine_last REVERSE)
  if(fbengine_first EQUAL -1 OR NOT fbengine_first EQUAL fbengine_last)
    message(FATAL_ERROR "fastgltf's ${fbengine_file} doesn't have, once, what a patch expects")
  endif()
  string(REPLACE "${fbengine_from}" "${fbengine_to}" fbengine_text "${fbengine_text}")
  file(WRITE "${fbengine_fastgltf_patched}/fastgltf/${fbengine_file}.in" "${fbengine_text}")
  configure_file("${fbengine_fastgltf_patched}/fastgltf/${fbengine_file}.in"
                 "${fbengine_fastgltf_patched}/fastgltf/${fbengine_file}" COPYONLY)
endforeach()
target_include_directories(fastgltf BEFORE PRIVATE "${fbengine_fastgltf_patched}")

# Its URIs too, in a noexcept constructor, where a URI of a scheme and an authority with nothing
# after it, as "x://y" or "x://", takes a substring from past its end, which ends the process, in
# a game's Load too. Nothing after the authority is an empty path.
file(READ "${fastgltf_SOURCE_DIR}/src/fastgltf.cpp" fbengine_text)
# Not a list, as a '[' in one keeps what follows it from being its next item.
function(fbengine_patch_text from to)
  string(FIND "${fbengine_text}" "${from}" first)
  string(FIND "${fbengine_text}" "${from}" last REVERSE)
  if(first EQUAL -1 OR NOT first EQUAL last)
    message(FATAL_ERROR "fastgltf.cpp doesn't have, once, what a patch expects: ${from}")
  endif()
  string(REPLACE "${from}" "${to}" patched "${fbengine_text}")
  set(fbengine_text "${patched}" PARENT_SCOPE)
endfunction()
fbengine_patch_text("\t\tif (view[idx] == '[') {" "\t\tif (idx < view.size() && view[idx] == '[') {")
fbengine_patch_text("\t\t\t++idx;\n\t\t\t_host = view.substr(idx, hostEnd - idx);"
                    "\t\t\tif (idx < view.size()) ++idx;\n\t\t\t_host = view.substr(idx, hostEnd - idx);")
fbengine_patch_text("\t\tidx = nextSlash; // Path includes this slash"
                    "\t\tidx = nextSlash == std::string::npos ? view.size() : nextSlash; // Path includes this slash")
file(WRITE "${fbengine_fastgltf_patched}/fastgltf.cpp.in" "${fbengine_text}")
configure_file("${fbengine_fastgltf_patched}/fastgltf.cpp.in"
               "${fbengine_fastgltf_patched}/fastgltf.cpp" COPYONLY)
fbengine_replace_source(fastgltf "${fastgltf_SOURCE_DIR}" src/fastgltf.cpp
                        "${fbengine_fastgltf_patched}/fastgltf.cpp")
target_include_directories(fastgltf PRIVATE "${fastgltf_SOURCE_DIR}/src")

# libktx's own zstd_errors.h is in a folder it includes as a system's, after what a Mac's compiler
# may find of Homebrew's zstd, whose header no longer has ZSTD_getErrorCode. A copy comes first in
# its targets, as a folder of its own, since CMake keeps a system's folder a system's.
set(fbengine_ktx_first "${CMAKE_BINARY_DIR}/ktx-first")
configure_file("${ktx_SOURCE_DIR}/other_include/zstd_errors.h"
               "${fbengine_ktx_first}/zstd_errors.h" COPYONLY)
target_include_directories(ktx BEFORE PRIVATE "${fbengine_ktx_first}")
target_include_directories(ktx_read BEFORE PRIVATE "${fbengine_ktx_first}")
# basisu's job pool, in libktx's encoder, which each UASTC cook makes one of, sets its kill flag
# without its mutex as it goes, so a worker between its wait's check and its wait misses the
# notify, and the pool's join waits for it forever, as a loaded machine's cooks do. The patched
# copy sets it under the mutex, and is built in libktx's writer in place of basisu_enc.cpp,
# finding its headers beside the original; the patch's text has to be there, once, or configuring
# fails.
set(fbengine_basisu_source "${ktx_SOURCE_DIR}/external/basisu/encoder/basisu_enc.cpp")
file(READ "${fbengine_basisu_source}" fbengine_basisu)
set(fbengine_basisu_from "\t\t// Notify all workers that they need to die right now.\n\t\tm_kill_flag = true;\n")
set(fbengine_basisu_to
    "\t\t// Notify all workers that they need to die right now.\n\t\t{\n\t\t\tstd::lock_guard<std::mutex> fbengine_lock(m_mutex);\n\t\t\tm_kill_flag = true;\n\t\t}\n")
string(FIND "${fbengine_basisu}" "${fbengine_basisu_from}" fbengine_first)
string(FIND "${fbengine_basisu}" "${fbengine_basisu_from}" fbengine_last REVERSE)
if(fbengine_first EQUAL -1 OR NOT fbengine_first EQUAL fbengine_last)
  message(FATAL_ERROR "basisu_enc.cpp doesn't have, once, what its patch expects")
endif()
string(REPLACE "${fbengine_basisu_from}" "${fbengine_basisu_to}" fbengine_basisu
               "${fbengine_basisu}")
set(fbengine_basisu_patched "${CMAKE_BINARY_DIR}/ktx-patched/basisu_enc.cpp")
file(WRITE "${fbengine_basisu_patched}.in" "${fbengine_basisu}")
configure_file("${fbengine_basisu_patched}.in" "${fbengine_basisu_patched}" COPYONLY)
fbengine_replace_source(ktx "${ktx_SOURCE_DIR}" external/basisu/encoder/basisu_enc.cpp
                        "${fbengine_basisu_patched}")
target_include_directories(ktx PRIVATE "${ktx_SOURCE_DIR}/external/basisu/encoder")

# fbengine's DLLs are UCRT's (cmake/native.cmake), so libktx is built for UCRT too: its encoder's
# sprintf_s is UCRT's, inlined from the headers, which msvcrt's has no export of. What links the
# writer links UCRT, as the assets tests do.
if(MINGW)
  foreach(target IN ITEMS ktx_read ktx objUtil astcenc-sse4.1-static)
    if(TARGET ${target})
      target_compile_definitions(${target} PRIVATE _UCRT)
    endif()
  endforeach()
endif()

# meshoptimizer, which the cooker orders meshes' triangles and vertices for the GPU with.
FetchContent_Declare(
  meshoptimizer
  GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
  GIT_TAG v1.3
  GIT_SHALLOW TRUE
  SYSTEM
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(meshoptimizer)
set_target_properties(meshoptimizer PROPERTIES POSITION_INDEPENDENT_CODE ON)
