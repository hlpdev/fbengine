# Slack Water as a player has it: a release built from a copy of the game, its executable and pak put
# in a folder of their own, the copy taken away, so nothing's left to read but those, and run from
# there. Takes FB, SOURCE, PACKAGES, WORK, SUFFIX, and SKIP_PAK, which leaves the pak out, to see it
# fail.
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
file(WRITE "${game}/project.toml" "${project}")

set(executable "${shipped}/slack_water${SUFFIX}")
execute_process(COMMAND "${FB}" --color never build --release "${game}" -o "${executable}"
                RESULT_VARIABLE built)
if(NOT built EQUAL 0)
  message(FATAL_ERROR "the release didn't build: ${built}")
endif()
if(NOT SKIP_PAK)
  file(COPY "${game}/build/slack_water.pak" DESTINATION "${shipped}")
endif()
file(REMOVE_RECURSE "${game}")

execute_process(COMMAND "${executable}" --cooked "${SOURCE}/tests/walk3d_bright.txt" "${WORK}/frames"
                WORKING_DIRECTORY "${shipped}" RESULT_VARIABLE ran)
if(NOT ran EQUAL 0)
  message(FATAL_ERROR "the shipped game failed: ${ran}")
endif()
message(STATUS "the shipped game ran from its own folder")
