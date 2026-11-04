# Runs a proof, and says how it ended before what it printed, so its test's lines still end its output,
# and a program that ends badly after them fails: `exit code N`, which a test's FAIL_REGULAR_EXPRESSION
# looks for. What it printed goes through a file, since it can be more than a command line can hold.
string(RANDOM LENGTH 16 name)
set(said "${CMAKE_CURRENT_BINARY_DIR}/proof-${name}.txt")
execute_process(COMMAND "${FB}" --color never run "${FOLDER}" ${ARGUMENTS}
                RESULT_VARIABLE ended OUTPUT_FILE "${said}" ERROR_FILE "${said}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "exit code ${ended}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E cat "${said}")
file(REMOVE "${said}")
