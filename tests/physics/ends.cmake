# Runs PROGRAM, which makes a world on Jolt's most threads and ends at once, a few at a time for
# RUNS rounds, so they start their threads on a busy machine, and fails if one doesn't end within
# 20 seconds, or ends any way but with 101, as it says it did. How many at a time is
# $ENV{FBENGINE_ENDS_AT_ONCE}, or 2: CI's 8, at which fbengine_physics 5350470's job threads hung
# about once in 300, is too many beside other test runs on a person's machine. DLLS is where its
# DLL is, which Windows finds on the PATH.
set(AT_ONCE 2)
if(DEFINED ENV{FBENGINE_ENDS_AT_ONCE})
  set(AT_ONCE "$ENV{FBENGINE_ENDS_AT_ONCE}")
endif()
if(WIN32)
  set(ENV{PATH} "${DLLS};$ENV{PATH}")
endif()
set(commands "")
foreach(i RANGE 1 ${AT_ONCE})
  list(APPEND commands COMMAND "${PROGRAM}")
endforeach()
foreach(run RANGE 1 ${RUNS})
  # Commands together are a pipeline, which runs them at once.
  execute_process(${commands} RESULTS_VARIABLE codes TIMEOUT 20 OUTPUT_QUIET
                  ERROR_VARIABLE err)
  list(LENGTH codes count)
  list(REMOVE_ITEM codes 101)
  if(NOT count EQUAL AT_ONCE OR codes)
    message(FATAL_ERROR "round ${run} of ${RUNS} ended with ${codes}, not 101 each:\n${err}")
  endif()
endforeach()
