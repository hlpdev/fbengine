# Runs `fb run PROGRAM -- CASE` and fails unless it exits with CODE, as a program that panics does
# with 101 and one that crashes doesn't.
execute_process(COMMAND "${FB}" --color never run "${PROGRAM}" -- "${CASE}"
                RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT code STREQUAL "${CODE}")
  message(FATAL_ERROR "${CASE} exited with ${code}, not ${CODE}:\n${out}${err}")
endif()
