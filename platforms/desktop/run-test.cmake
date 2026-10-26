# Own temporary files outside the test process: standalone checks can exit or
# abort before their QTemporaryDir destructors run.
if(NOT EXISTS "${TEST_EXECUTABLE}" OR NOT IS_DIRECTORY "${TEST_TMP_ROOT}")
    message(FATAL_ERROR "A test executable and temporary root are required")
endif()
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef run)
set(work "${TEST_TMP_ROOT}/kobalt-regression-${run}")
# Fast checks default to 30 seconds. Integration matrices and the screenshot
# run pass longer limits. CTest leaves a cleanup margin outside this guard.
if(NOT DEFINED TEST_TIMEOUT)
    set(TEST_TIMEOUT 30)
endif()
if(EXISTS "${work}")
    message(FATAL_ERROR "Test temporary directory already exists: ${work}")
endif()
file(MAKE_DIRECTORY "${work}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "TMPDIR=${work}" "${TEST_EXECUTABLE}" ${TEST_ARGUMENTS}
    WORKING_DIRECTORY "${work}"
    RESULT_VARIABLE result
    TIMEOUT ${TEST_TIMEOUT})
file(REMOVE_RECURSE "${work}")
if(NOT result STREQUAL "0")
    message(FATAL_ERROR "Regression test failed: ${result}")
endif()
