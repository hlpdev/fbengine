# fbengine_audio's own DLL, which the fbengine_audio package's FB code calls through
# fbengine_audio.h, an area of its C API to a folder: audio/, and the ones each step adds.

# miniaudio, compiled here into a static library of its own, without its warnings, as it ships no
# CMake build fbengine wants: SOURCE_SUBDIR names a folder it doesn't have, so only its sources
# come. The engine loads OGG, WAV, and FLAC, never MP3, and writes no sound files.
FetchContent_Declare(
  miniaudio
  GIT_REPOSITORY https://github.com/mackron/miniaudio.git
  GIT_TAG 0.11.26
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR none
  SYSTEM
)
FetchContent_MakeAvailable(miniaudio)
add_library(fbengine_miniaudio STATIC "${miniaudio_SOURCE_DIR}/miniaudio.c")
target_include_directories(fbengine_miniaudio SYSTEM PUBLIC "${miniaudio_SOURCE_DIR}")
target_compile_definitions(fbengine_miniaudio PUBLIC MA_NO_MP3 MA_NO_ENCODING)
set_target_properties(fbengine_miniaudio PROPERTIES POSITION_INDEPENDENT_CODE ON)
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
