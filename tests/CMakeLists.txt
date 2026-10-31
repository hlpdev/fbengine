# The packages' own tests, run by `fb test`, and the examples, run by `fb run` and checked against
# what they print, each after the DLLs are built into the packages' native/<target>/ folders.
find_program(FBENGINE_FB fb REQUIRED DOC "The fb that builds and tests the packages")

add_test(NAME fbengine.package
         COMMAND "${FBENGINE_FB}" test "${PROJECT_SOURCE_DIR}/packages/fbengine")

# fb's own lines, building the program, come before what it prints.
add_test(NAME examples.hello
         COMMAND "${FBENGINE_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/hello")
set_tests_properties(examples.hello PROPERTIES
                     PASS_REGULAR_EXPRESSION "Running hello[^\n]*\nfbengine ${PROJECT_VERSION}\r?\n$")

# The tests use what the last build put in the packages.
add_custom_target(fbengine_natives ALL DEPENDS fbengine_native)
