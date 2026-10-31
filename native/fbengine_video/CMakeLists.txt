# fbengine_video's DLL, which the fbengine_video package's FB code calls through fbengine_video.h,
# an area of its C API to a folder: video/, and the ones E12 adds. pl_mpeg decodes MPEG-1 in it.
FetchContent_Declare(
  pl_mpeg
  GIT_REPOSITORY https://github.com/phoboslab/pl_mpeg.git
  # It has no releases; this is its commit of 2025's "Fix corrupt slice check".
  GIT_TAG c871f2be022ece7ef4f64230b4fb8e1fb9eb6023
  SYSTEM
)
FetchContent_MakeAvailable(pl_mpeg)
# Its one header, built once, as C, apart from the DLL's own warnings; it takes stdio.h's FILE.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/pl_mpeg.c"
     "#include <stdio.h>\n#define PL_MPEG_IMPLEMENTATION\n#include \"pl_mpeg.h\"\n")
add_library(pl_mpeg STATIC "${CMAKE_CURRENT_BINARY_DIR}/pl_mpeg.c")
target_include_directories(pl_mpeg SYSTEM PUBLIC "${pl_mpeg_SOURCE_DIR}")
set_target_properties(pl_mpeg PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 11)

find_package(Threads REQUIRED)
file(GLOB fbengine_video_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*/*.cpp")
fbengine_add_native(fbengine_video_native fbengine_video fbe_ ${fbengine_video_sources})
target_include_directories(fbengine_video_native PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(fbengine_video_native PRIVATE FBE_VIDEO_BUILDING
                           FBE_VERSION="${PROJECT_VERSION}")
target_link_libraries(fbengine_video_native PRIVATE pl_mpeg Threads::Threads)
