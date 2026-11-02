# Runs the hidden test that fails after its golden image check, updating, as tools/golden.py does,
# and fails if the image was written: an update takes only a passing test's images.
#
#   cmake -DTESTS=<fbengine_gpu_tests> -DIMAGE=<its golden image> -P failed_update.cmake

if(EXISTS "${IMAGE}")
  message(FATAL_ERROR "${IMAGE} is there before the run, so this checks nothing; delete it")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env FBENGINE_GOLDEN=update "${TESTS}"
          "a golden image test that fails after its check"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE output)
if(result EQUAL 0)
  message(FATAL_ERROR "the test that fails after its check passed:\n${output}")
endif()
if(EXISTS "${IMAGE}")
  file(REMOVE "${IMAGE}")
  message(FATAL_ERROR "a failed test's update wrote ${IMAGE}:\n${output}")
endif()
message(STATUS "the failed test left ${IMAGE} unwritten")
