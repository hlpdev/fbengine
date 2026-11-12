# Runs a proof, and says how it ended before what it printed, so its test's lines still end its output,
# and a program that ends badly after them fails: `exit code N`, which a test's FAIL_REGULAR_EXPRESSION
# looks for. What it printed goes through a file, since it can be more than a command line can hold.
# And whether Vulkan's validation layers were on, which the GPU says as it starts when it's asked,
# and this says before what the proof printed, so a run they found nothing in isn't taken for one
# they checked when they weren't there; each thing they find is a line a test's
# FAIL_REGULAR_EXPRESSION looks for, `Vulkan validation error`.
string(RANDOM LENGTH 16 name)
set(said "${CMAKE_CURRENT_BINARY_DIR}/proof-${name}.txt")
set(ENV{FBE_GPU_VALIDATION_SAID} 1)
execute_process(COMMAND "${FB}" --color never run "${FOLDER}" ${ARGUMENTS}
                RESULT_VARIABLE ended OUTPUT_FILE "${said}" ERROR_FILE "${said}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "exit code ${ended}")
file(STRINGS "${said}" layers REGEX "^fbengine: Vulkan's validation layers are (on|off)$")
if(layers MATCHES "are on")
  execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "validation layers: on")
elseif(layers MATCHES "are off")
  execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "validation layers: off")
else()
  execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "validation layers: no GPU was started")
endif()
# Its line's taken out of what the proof printed, which a test's lines are matched in a row.
file(READ "${said}" printed)
string(REGEX REPLACE "fbengine: Vulkan's validation layers are (on|off)\r?\n" "" printed "${printed}")
file(WRITE "${said}" "${printed}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E cat "${said}")
file(REMOVE "${said}")
