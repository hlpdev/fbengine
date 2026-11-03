# Runs a proof, and says how it ended before what it printed, so its test's lines still end its output
# and a program that ends badly after them fails: `exit code N`, which a test's FAIL_REGULAR_EXPRESSION
# looks for.
execute_process(COMMAND "${FB}" --color never run "${FOLDER}" ${ARGUMENTS}
                RESULT_VARIABLE ended OUTPUT_VARIABLE said ERROR_VARIABLE said)
execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "exit code ${ended}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E echo_append "${said}")
