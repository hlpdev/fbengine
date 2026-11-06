# fbengine_audio's own DLL, which the fbengine_audio package's FB code calls through
# fbengine_audio.h, an area of its C API to a folder: audio/, and the ones each step adds.

# miniaudio, compiled here into a static library of its own, without its warnings, as it ships no
# CMake build fbengine wants: SOURCE_SUBDIR names a folder it doesn't have, so only its sources
# come. The engine loads OGG, WAV, and FLAC, never MP3, and writes no sound files; Ogg Vorbis is
# stb_vorbis, from miniaudio's extras, which its implementation finds declared before it. Its
# extras' reverb node, verblib's Freeverb, is the buses' reverb.
FetchContent_Declare(
  miniaudio
  GIT_REPOSITORY https://github.com/mackron/miniaudio.git
  GIT_TAG 0.11.26
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR none
  SYSTEM
)
FetchContent_MakeAvailable(miniaudio)

# stb_vorbis 1.22, patched where a corrupt file corrupts the heap, leaks, or shifts a negative:
# - The vendor's and each comment's length and the comment count are read from the file and size
#   allocations, through setup_malloc's int, which a count past 2^28 overflows; a comment that
#   won't read leaves comment_list partly unset, and a count too big to allocate leaves it null,
#   for vorbis_deinit to free what was never allocated either way. The copy refuses a string past
#   16 MB or a count past 2^20, clears comment_list as it's allocated, and frees its comments only
#   when there's a list.
# - A sparse codebook's lengths, up to 16 MB, and its values are temporary allocations, which an
#   error return between their malloc and free leaks. The copy keeps them in the decoder, clears
#   them as they're freed, and frees what's left in vorbis_deinit (a codebook's codewords, which
#   are temporary too, it frees already).
# - get32_packet shifts get8_packet's -1, at a packet's end, left; the copy shifts it as a uint32.
# - Each decoder's start makes the CRC table again, in place, while the mix's thread reads it for a
#   stream it seeks: a data race (ThreadSanitizer), in stb_vorbis itself, v1.22 as nothings/stb and
#   miniaudio 0.11.26's extras have it, whose start_decoder calls crc32_init (line 992) at line
#   3707, "to avoid multithread race conditions". The copy makes it only in
#   fbengine_vorbis_crc32_init, which fbengine_audio calls once, before its first decoder
#   (sound/sound.cpp's ready_decoders).
# Each patch's text has to be there, once, or configuring fails.
file(READ "${miniaudio_SOURCE_DIR}/extras/stb_vorbis.c" fbengine_stb_vorbis)
set(fbengine_stb_vorbis_patches
    "   len = get32_packet(f)\;\n   f->vendor ="
    "   len = get32_packet(f)\;\n   if (len < 0 || len > (1 << 24))                 return error(f, VORBIS_invalid_setup)\;\n   f->vendor ="
    "   f->comment_list_length = get32_packet(f)\;\n"
    "   f->comment_list_length = get32_packet(f)\;\n   if (f->comment_list_length < 0 || f->comment_list_length > (1 << 20)) return error(f, VORBIS_invalid_setup)\;\n"
    "      if (f->comment_list == NULL)                  return error(f, VORBIS_outofmem)\;\n"
    "      if (f->comment_list == NULL)                  return error(f, VORBIS_outofmem)\;\n      memset(f->comment_list, 0, sizeof(char*) * (f->comment_list_length))\;\n"
    "      len = get32_packet(f)\;\n      f->comment_list[i] ="
    "      len = get32_packet(f)\;\n      if (len < 0 || len > (1 << 24))              return error(f, VORBIS_invalid_setup)\;\n      f->comment_list[i] ="
    "   for (i=0\; i < p->comment_list_length\; ++i) {\n"
    "   for (i=0\; p->comment_list != NULL && i < p->comment_list_length\; ++i) {\n"
    "   int temp_offset\;\n"
    "   int temp_offset\;\n   void *fbe_temp_lengths, *fbe_temp_values\;\n"
    "         lengths = (uint8 *) setup_temp_malloc(f, c->entries)\;\n"
    "         lengths = (uint8 *) (f->fbe_temp_lengths = setup_temp_malloc(f, c->entries))\;\n"
    "            values = (uint32 *) setup_temp_malloc(f, sizeof(*values) * c->sorted_entries)\;\n"
    "            values = (uint32 *) (f->fbe_temp_values = setup_temp_malloc(f, sizeof(*values) * c->sorted_entries))\;\n"
    "      f->temp_offset += (sz+7)&~7\;\n      return\;\n   }\n   free(p)\;\n"
    "      f->temp_offset += (sz+7)&~7\;\n      return\;\n   }\n   if (p == f->fbe_temp_lengths) f->fbe_temp_lengths = NULL\;\n   if (p == f->fbe_temp_values) f->fbe_temp_values = NULL\;\n   free(p)\;\n"
    "   int i,j\;\n\n   setup_free(p, p->vendor)\;\n"
    "   int i,j\;\n\n   if (!p->alloc.alloc_buffer) {\n      free(p->fbe_temp_lengths)\;\n      free(p->fbe_temp_values)\;\n   }\n   setup_free(p, p->vendor)\;\n"
    "   x += get8_packet(f) << 8\;\n   x += get8_packet(f) << 16\;\n"
    "   x += (uint32) get8_packet(f) << 8\;\n   x += (uint32) get8_packet(f) << 16\;\n"
    "static void crc32_init(void)\n{"
    "void fbengine_vorbis_crc32_init(void)\n{"
    "   crc32_init()\; // always init it, to avoid multithread race conditions\n"
    "   // fbengine_audio makes the CRC table once, before its first decoder.\n")
list(LENGTH fbengine_stb_vorbis_patches fbengine_patch_count)
math(EXPR fbengine_patch_last "${fbengine_patch_count} - 1")
foreach(fbengine_patch RANGE 0 ${fbengine_patch_last} 2)
  math(EXPR fbengine_patched "${fbengine_patch} + 1")
  list(GET fbengine_stb_vorbis_patches ${fbengine_patch} fbengine_from)
  list(GET fbengine_stb_vorbis_patches ${fbengine_patched} fbengine_to)
  string(FIND "${fbengine_stb_vorbis}" "${fbengine_from}" fbengine_first)
  string(FIND "${fbengine_stb_vorbis}" "${fbengine_from}" fbengine_last REVERSE)
  if(fbengine_first EQUAL -1 OR NOT fbengine_first EQUAL fbengine_last)
    message(FATAL_ERROR "stb_vorbis.c doesn't have, once, what a patch expects: ${fbengine_from}")
  endif()
  string(REPLACE "${fbengine_from}" "${fbengine_to}" fbengine_stb_vorbis "${fbengine_stb_vorbis}")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/stb_vorbis.c.in" "${fbengine_stb_vorbis}")
configure_file("${CMAKE_CURRENT_BINARY_DIR}/stb_vorbis.c.in" "${CMAKE_CURRENT_BINARY_DIR}/stb_vorbis.c"
               COPYONLY)

set(fbengine_miniaudio_source "${CMAKE_CURRENT_BINARY_DIR}/miniaudio.c")
file(WRITE "${fbengine_miniaudio_source}.in" [=[
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#undef STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#include "extras/nodes/ma_reverb_node/ma_reverb_node.c"
]=])
configure_file("${fbengine_miniaudio_source}.in" "${fbengine_miniaudio_source}" COPYONLY)
add_library(fbengine_miniaudio STATIC "${fbengine_miniaudio_source}")
target_include_directories(fbengine_miniaudio SYSTEM PUBLIC "${miniaudio_SOURCE_DIR}")
target_compile_definitions(fbengine_miniaudio PUBLIC MA_NO_MP3 MA_NO_ENCODING)
# miniaudio's COM on Windows is apartment-threaded, as SDL's OLE, which a window's drag and drop
# needs, is on the same thread; multithreaded, miniaudio's default, would make OleInitialize fail.
target_compile_definitions(fbengine_miniaudio PRIVATE MA_COINIT_VALUE=2)
set_target_properties(fbengine_miniaudio PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Its decoders add and multiply a file's samples and coefficients in signed ints, which a corrupt
# file overflows: dr_flac's prediction does, which is undefined, so they're made to wrap, as the
# code takes them to (tests/audio/fuzz_tests.cpp, and tools.miniaudio_wraps).
if(NOT MSVC)
  target_compile_options(fbengine_miniaudio PRIVATE -fwrapv)
endif()
find_package(Threads REQUIRED)
target_link_libraries(fbengine_miniaudio PUBLIC Threads::Threads)
if(APPLE)
  target_link_libraries(fbengine_miniaudio PUBLIC "-framework CoreFoundation"
                        "-framework CoreAudio" "-framework AudioToolbox")
elseif(UNIX)
  # Linux's backends (ALSA, PulseAudio, JACK) are loaded as it starts, so it needs only dlopen.
  target_link_libraries(fbengine_miniaudio PUBLIC ${CMAKE_DL_LIBS} m)
endif()

file(GLOB fbengine_audio_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*/*.cpp")
fbengine_add_native(fbengine_audio_native fbengine_audio fbe_ ${fbengine_audio_sources})
target_include_directories(fbengine_audio_native PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(fbengine_audio_native PRIVATE FBE_AUDIO_BUILDING
                           FBE_VERSION="${PROJECT_VERSION}")
target_link_libraries(fbengine_audio_native PRIVATE fbengine_miniaudio)
# Its float arithmetic as written, never fused: a multiply-subtract fused near the speed of sound
# rounds once where the Doppler effect's tests of it round twice.
if(NOT MSVC)
  target_compile_options(fbengine_audio_native PRIVATE -ffp-contract=off)
endif()
