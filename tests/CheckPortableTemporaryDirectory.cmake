# Exercise the tests with a runner-provided temp root, with no /tmp/opencode
# prerequisite. Keep it short enough for the Qt test's Unix socket path.
file(MAKE_DIRECTORY "${TEST_TEMP_ROOT}")
foreach(test IN ITEMS "${CONFIG_TEST}" "${EDITOR_TEST}" "${WINDOW_TEST}")
  if (NOT test STREQUAL "")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E env "TMPDIR=${TEST_TEMP_ROOT}"
        "QT_QPA_PLATFORM=offscreen" "${test}"
      RESULT_VARIABLE result
      OUTPUT_VARIABLE output
      ERROR_VARIABLE error
    )
    if (NOT result EQUAL 0)
      message(FATAL_ERROR "Test failed with custom TMPDIR: ${test}\n${output}\n${error}")
    endif ()
  endif ()
endforeach()
