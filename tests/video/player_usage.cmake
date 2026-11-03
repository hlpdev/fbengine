# The video player example, given anything but how it's run: each says how it's run, and fails, with
# no window, as ctest gives it offscreen video.
#
#   cmake -DFB=<fb> -DEXAMPLE=<folder> -P player_usage.cmake
foreach(given IN ITEMS "a" "--watch|a" "--steps|5" "--watch|--steps" "--watch|--steps|0"
                      "--watch|--steps|-1" "--watch|--steps|a" "--watch|--steps|5|a")
  string(REPLACE "|" ";" arguments "${given}")
  execute_process(COMMAND "${FB}" --color never run "${EXAMPLE}" -- ${arguments}
                  RESULT_VARIABLE code OUTPUT_VARIABLE printed ERROR_VARIABLE printed)
  if(code EQUAL 0)
    message(FATAL_ERROR "`${given}` ended well:\n${printed}")
  endif()
  if(NOT printed MATCHES "usage: video_player \\[--watch \\[--steps <n>\\]\\]")
    message(FATAL_ERROR "`${given}` didn't say how it's run:\n${printed}")
  endif()
endforeach()
