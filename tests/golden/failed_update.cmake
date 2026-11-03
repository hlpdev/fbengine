# Runs the hidden test that fails after its golden image check, updating, as tools/golden.py does,
# and fails if the image was written: an update takes only a passing test's images. It's run itself,
# with a timeout of its own, since ctest's kills only this script, and what it started would keep
# ctest waiting on its pipes for good. And it ends having stopped the GPU the failed test left.
#
#   cmake -DTESTS=<fbengine_gpu_tests> -DIMAGE=<its golden image> -P failed_update.cmake

if(EXISTS "${IMAGE}")
  message(FATAL_ERROR "${IMAGE} is there before the run, so this checks nothing; delete it")
endif()
set(ENV{FBENGINE_GOLDEN} update)
execute_process(
  COMMAND "${TESTS}" "a golden image test that fails after its check"
  TIMEOUT 120
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE output)
if(result MATCHES "timeout")
  message(FATAL_ERROR "the test that fails after its check never ended:\n${output}")
endif()
if(result EQUAL 0)
  message(FATAL_ERROR "the test that fails after its check passed:\n${output}")
endif()
if(NOT output MATCHES "stopped the GPU a test left running")
  message(FATAL_ERROR "the test that fails after its check left the GPU running:\n${output}")
endif()
if(EXISTS "${IMAGE}")
  file(REMOVE "${IMAGE}")
  message(FATAL_ERROR "a failed test's update wrote ${IMAGE}:\n${output}")
endif()
message(STATUS "the failed test left ${IMAGE} unwritten")
