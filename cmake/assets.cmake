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
FetchContent_Declare(
  KTX
  GIT_REPOSITORY https://github.com/KhronosGroup/KTX-Software.git
  GIT_TAG v4.4.2
  GIT_SHALLOW TRUE
  SYSTEM
  EXCLUDE_FROM_ALL
)

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

add_library(fbengine_stb INTERFACE)
target_include_directories(fbengine_stb SYSTEM INTERFACE "${stb_SOURCE_DIR}")

# Built into a shared library, their code is position-independent too.
set_target_properties(ktx_read ktx fastgltf PROPERTIES POSITION_INDEPENDENT_CODE ON)

# MinGW's off_t, which libktx's offsets are, is 32-bit unless asked for 64, which libktx's debug
# checks and its files over 2 GB need; what includes ktx.h sees the same.
if(MINGW)
  target_compile_definitions(ktx_read PUBLIC _FILE_OFFSET_BITS=64)
  target_compile_definitions(ktx PUBLIC _FILE_OFFSET_BITS=64)
endif()
