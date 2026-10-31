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
