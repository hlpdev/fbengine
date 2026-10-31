# Runs PROGRAM, which makes a world on Jolt's most threads and ends at once, AT_ONCE at a time
# for RUNS rounds, so they start their threads on a busy machine, and fails if one doesn't end
# within 20 seconds, or ends any way but with 101, as it says it did. With fbengine_physics 5350470's
# job threads, 8 at a time hung about once in 300. DLLS is where its DLL is, which Windows finds on
# the PATH.
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
