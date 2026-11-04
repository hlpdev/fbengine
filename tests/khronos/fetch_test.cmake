# fetch.cmake's three ways, none of which needs the network: `cmake -P`, in a folder of its own, with
# FOLDER set. Run with WRONG set, it fetches what isn't what its hash says, which is fatal.
include("${CMAKE_CURRENT_LIST_DIR}/fetch.cmake")
file(MAKE_DIRECTORY "${FOLDER}")
file(WRITE "${FOLDER}/source.txt" "what's pinned")
file(SHA256 "${FOLDER}/source.txt" pinned)
if(WRONG)
  fbengine_fetch_khronos("file://${FOLDER}/source.txt" "${FOLDER}/wrong.txt" "${pinned}0" FALSE fetched)
  message(STATUS "a wrong hash went by")
  return()
endif()
# Unreachable: nothing listens on port 9.
file(REMOVE "${FOLDER}/gone.txt")
fbengine_fetch_khronos("http://127.0.0.1:9/gone.txt" "${FOLDER}/gone.txt" "${pinned}" FALSE fetched)
if(fetched OR EXISTS "${FOLDER}/gone.txt")
  message(FATAL_ERROR "what can't be reached was taken as fetched")
endif()
# What's there, by its hash.
file(REMOVE "${FOLDER}/got.txt")
fbengine_fetch_khronos("file://${FOLDER}/source.txt" "${FOLDER}/got.txt" "${pinned}" FALSE fetched)
if(NOT fetched OR NOT EXISTS "${FOLDER}/got.txt")
  message(FATAL_ERROR "what's there wasn't fetched")
endif()
message(STATUS "fetch's ways are as they should be")
