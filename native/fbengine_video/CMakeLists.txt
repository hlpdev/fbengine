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

# pl_mpeg, patched where a corrupt video reads or writes outside the heap, shifts a negative, or
# never shows its last picture:
# - A macroblock's address can go negative, which only its end was checked against, and a motion
#   vector's block was checked by where it starts, not all it reads and writes: its pull request
#   #74's checks of both, and of an audio frame's bit rate index of 0, its issue #68.
# - An audio frame's sync is looked for up to its buffer's length less 1, which is a size_t that
#   an empty buffer wraps: its issue #69. And a search that finds none, from the end of a buffer
#   that ends in zeros, leaves its place a byte past the end, which what's left of the buffer
#   wraps; the copy leaves it at the end.
# - Motion vectors of full pixels, and an intra block's DC, are shifted left while negative, which
#   C leaves undefined; the copy shifts them as unsigned. A packet's time's top 3 bits are shifted
#   left 30 as an int, which overflows past 2^31, 6.6 hours; the copy shifts them as an int64_t.
# - At its end, a video's last reference picture is shown only if the last picture decoded was a
#   reference too, so one whose stream ends with B-frames never shows its last; the copy shows it
#   whatever the last was.
# Each patch's text has to be there, once, or configuring fails.
file(READ "${pl_mpeg_SOURCE_DIR}/pl_mpeg.h" fbengine_pl_mpeg)
# As git checks it out, which may be with CRLFs.
string(REPLACE "\r\n" "\n" fbengine_pl_mpeg "${fbengine_pl_mpeg}")
set(fbengine_pl_mpeg_patches
    "\tif (self->mb_col >= self->mb_width || self->mb_row >= self->mb_height) {\n\t\treturn\; // corrupt stream\;"
    "\tif (self->mb_col < 0 || self->mb_col >= self->mb_width ||\n\t\tself->mb_row < 0 || self->mb_row >= self->mb_height) {\n\t\treturn\; // corrupt stream\;"
    "\tunsigned int max_address = (dw * (self->mb_height * block_size - block_size + 1) - block_size)\;\n\tif (si > max_address || di > max_address) {"
    "\tunsigned int plane_size = (unsigned int)dw * (self->mb_height * block_size)\;\n\tunsigned int block_span = (unsigned int)(block_size - 1) * dw + (block_size - 1)\;\n\tunsigned int src_extra = (odd_v ? (unsigned int)dw : 0) + (odd_h ? 1u : 0)\;\n\tif (si >= plane_size || di >= plane_size ||\n\t\tsi + block_span + src_extra >= plane_size ||\n\t\tdi + block_span >= plane_size) {"
    "\tint bitrate_index = plm_buffer_read(self->buffer, 4) - 1\;\n\tif (bitrate_index > 13) {"
    "\tint bitrate_index = plm_buffer_read(self->buffer, 4) - 1\;\n\tif (bitrate_index < 0 || bitrate_index > 13) {"
    "\tfor (i = self->buffer->bit_index >> 3\; i < self->buffer->length-1\; i++) {"
    "\tfor (i = self->buffer->bit_index >> 3\; i + 1 < self->buffer->length\; i++) {"
    "\t\tfw_h <<= 1\;\n\t\tfw_v <<= 1\;"
    "\t\tfw_h = (int)((unsigned int)fw_h << 1)\;\n\t\tfw_v = (int)((unsigned int)fw_v << 1)\;"
    "\t\t\tbw_h <<= 1\;\n\t\t\tbw_v <<= 1\;"
    "\t\t\tbw_h = (int)((unsigned int)bw_h << 1)\;\n\t\t\tbw_v = (int)((unsigned int)bw_v << 1)\;"
    "\t\tself->block_data[0] <<= (3 + 5)\;"
    "\t\tself->block_data[0] = (int)((unsigned int)self->block_data[0] << (3 + 5))\;"
    "\tint64_t clock = plm_buffer_read(self->buffer, 3) << 30\;"
    "\tint64_t clock = (int64_t)plm_buffer_read(self->buffer, 3) << 30\;"
    "\t\t\t\t\tplm_buffer_has_ended(self->buffer) && (\n\t\t\t\t\t\tself->picture_type == PLM_VIDEO_PICTURE_TYPE_INTRA ||\n\t\t\t\t\t\tself->picture_type == PLM_VIDEO_PICTURE_TYPE_PREDICTIVE\n\t\t\t\t\t)\n\t\t\t\t) {"
    "\t\t\t\t\tplm_buffer_has_ended(self->buffer)\n\t\t\t\t) {"
    "\tself->buffer->bit_index = (i + 1) << 3\;\n\treturn FALSE\;"
    "\tself->buffer->bit_index = (i + 1 < self->buffer->length ? i + 1 : self->buffer->length) << 3\;\n\treturn FALSE\;")
list(LENGTH fbengine_pl_mpeg_patches fbengine_patch_count)
math(EXPR fbengine_patch_last "${fbengine_patch_count} - 1")
foreach(fbengine_patch RANGE 0 ${fbengine_patch_last} 2)
  math(EXPR fbengine_patched "${fbengine_patch} + 1")
  list(GET fbengine_pl_mpeg_patches ${fbengine_patch} fbengine_from)
  list(GET fbengine_pl_mpeg_patches ${fbengine_patched} fbengine_to)
  string(FIND "${fbengine_pl_mpeg}" "${fbengine_from}" fbengine_first)
  string(FIND "${fbengine_pl_mpeg}" "${fbengine_from}" fbengine_last REVERSE)
  if(fbengine_first EQUAL -1 OR NOT fbengine_first EQUAL fbengine_last)
    message(FATAL_ERROR "pl_mpeg.h doesn't have, once, what a patch expects: ${fbengine_from}")
  endif()
  string(REPLACE "${fbengine_from}" "${fbengine_to}" fbengine_pl_mpeg "${fbengine_pl_mpeg}")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/pl_mpeg.h.in" "${fbengine_pl_mpeg}")
configure_file("${CMAKE_CURRENT_BINARY_DIR}/pl_mpeg.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/pl_mpeg/pl_mpeg.h" COPYONLY)

# Its one header, built once, as C, apart from the DLL's own warnings; it takes stdio.h's FILE.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/pl_mpeg.c.in"
     "#include <stdio.h>\n#define PL_MPEG_IMPLEMENTATION\n#include \"pl_mpeg.h\"\n")
configure_file("${CMAKE_CURRENT_BINARY_DIR}/pl_mpeg.c.in" "${CMAKE_CURRENT_BINARY_DIR}/pl_mpeg.c"
               COPYONLY)
add_library(pl_mpeg STATIC "${CMAKE_CURRENT_BINARY_DIR}/pl_mpeg.c")
target_include_directories(pl_mpeg SYSTEM PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/pl_mpeg")
set_target_properties(pl_mpeg PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 11)

find_package(Threads REQUIRED)
file(GLOB fbengine_video_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*/*.cpp")
fbengine_add_native(fbengine_video_native fbengine_video fbe_ ${fbengine_video_sources})
target_include_directories(fbengine_video_native PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(fbengine_video_native PRIVATE FBE_VIDEO_BUILDING
                           FBE_VERSION="${PROJECT_VERSION}")
target_link_libraries(fbengine_video_native PRIVATE pl_mpeg Threads::Threads)
