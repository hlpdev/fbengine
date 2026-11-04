# Slack Water as a player has it: a release built from a copy of the game, its executable and pak put
# in a folder of their own, the copy taken away, so nothing's left to read but those, and run from
# there. Takes FB, SOURCE, PACKAGES, WORK, and SUFFIX; BUNDLE, to lay it out as a macOS bundle; and
# PAK, as MISSING, BROKEN, or OLD, another version's, to run it as a player would, with no arguments,
# which says what's wrong before it opens a window, and fails. MISSING leaves the pak where it was built,
# which a player's machine wouldn't have, and it mustn't read.
unset(ENV{FUTUREBLITZ_RUN})
set(game "${WORK}/game")
set(shipped "${WORK}/shipped")
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${game}" "${shipped}")

# The game's source and assets, its dependencies by where they really are.
file(GLOB sources "${SOURCE}/*.fb")
file(COPY ${sources} DESTINATION "${game}")
file(COPY "${SOURCE}/assets" DESTINATION "${game}")
file(READ "${SOURCE}/project.toml" project)
string(REPLACE "../../packages" "${PACKAGES}" project "${project}")

set(from "${shipped}")
set(paks "${shipped}")
if(BUNDLE)
  set(from "${shipped}/Slack Water.app/Contents/MacOS")
  set(paks "${shipped}/Slack Water.app/Contents/Resources")
  file(MAKE_DIRECTORY "${from}" "${paks}")
endif()
set(executable "${from}/slack_water${SUFFIX}")

function(build_it)
  execute_process(COMMAND "${FB}" --color never build --release "${game}" -o "${executable}"
                  RESULT_VARIABLE built)
  if(NOT built EQUAL 0)
    message(FATAL_ERROR "the release didn't build: ${built}")
  endif()
endfunction()

if(PAK STREQUAL "OLD")
  # A pak of a version before this one's.
  string(REGEX REPLACE "version = \"[^\"]*\"" "version = \"0.0.1\"" older "${project}")
  file(WRITE "${game}/project.toml" "${older}")
  build_it()
  file(RENAME "${game}/build/slack_water.pak" "${WORK}/old.pak")
endif()
file(WRITE "${game}/project.toml" "${project}")
build_it()
if(PAK STREQUAL "OLD")
  file(COPY_FILE "${WORK}/old.pak" "${paks}/slack_water.pak")
elseif(PAK STREQUAL "BROKEN")
  file(WRITE "${paks}/slack_water.pak" "this isn't a pak\n")
elseif(NOT PAK STREQUAL "MISSING")
  file(COPY "${game}/build/slack_water.pak" DESTINATION "${paks}")
endif()
if(NOT PAK STREQUAL "MISSING")
  file(REMOVE_RECURSE "${game}")
endif()

if(PAK)
  execute_process(COMMAND "${executable}" WORKING_DIRECTORY "${from}" RESULT_VARIABLE ran)
  if(ran EQUAL 0)
    message(FATAL_ERROR "the shipped game ran with its pak ${PAK}")
  endif()
  return()
endif()

# What's in a folder it's told to save in stays, and it won't run its tests' modes.
set(frames "${WORK}/frames")
file(WRITE "${frames}/a player's.txt" "kept\n")
unset(ENV{SLACK_WATER_CHECKS})
execute_process(COMMAND "${executable}" --cooked "${SOURCE}/tests/walk3d_bright.txt" "${frames}"
                WORKING_DIRECTORY "${from}" RESULT_VARIABLE ran)
if(NOT ran EQUAL 0)
  message(FATAL_ERROR "the shipped game failed: ${ran}")
endif()
if(NOT EXISTS "${frames}/a player's.txt")
  message(FATAL_ERROR "the shipped game took what was in the folder it was told to save in")
endif()
if(NOT EXISTS "${frames}/island.png")
  message(FATAL_ERROR "the shipped game didn't save its views")
endif()
execute_process(COMMAND "${executable}" --frames "${WORK}/frames_no" WORKING_DIRECTORY "${from}"
                RESULT_VARIABLE ran ERROR_QUIET)
if(NOT ran EQUAL 2 OR EXISTS "${WORK}/frames_no")
  message(FATAL_ERROR "the shipped game ran a mode that's for its tests: ${ran}")
endif()
message(STATUS "the shipped game ran from its own folder")
