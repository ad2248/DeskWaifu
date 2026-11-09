cmake_minimum_required(VERSION 3.25)

file(MAKE_DIRECTORY "${FIXTURE_DIR}")
file(WRITE "${FIXTURE_DIR}/invalid.json" "{\"render_device\":false}")
file(WRITE "${FIXTURE_DIR}/invalid-address.json" "{\"render_device\":\"intel\"}")
file(WRITE "${FIXTURE_DIR}/valid.json" "{}")
foreach(config invalid invalid-address)
  execute_process(COMMAND "${APP}" --config "${FIXTURE_DIR}/${config}.json"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 5)
  if(NOT result EQUAL 1 OR NOT error MATCHES "render_device")
    message(FATAL_ERROR "Invalid render device config did not fail explicitly: ${result}: ${output} ${error}")
  endif()
endforeach()
execute_process(COMMAND "${APP}" --config "${FIXTURE_DIR}/valid.json" --render-device intel
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 5)
if(NOT result EQUAL 1 OR NOT error MATCHES "Invalid render device")
  message(FATAL_ERROR "Invalid render device CLI did not fail explicitly: ${result}: ${output} ${error}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "DRI_PRIME=1"
  "${APP}" --config "${FIXTURE_DIR}/valid.json" --render-device 0000:00:02.0
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 5)
if(NOT result EQUAL 1 OR NOT error MATCHES "Unset")
  message(FATAL_ERROR "Conflicting driver override was not rejected: ${result}: ${output} ${error}")
endif()
execute_process(COMMAND "${APP}" --render-device
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 5)
if(NOT result EQUAL 1 OR NOT error MATCHES "requires")
  message(FATAL_ERROR "Missing render device CLI value did not fail explicitly: ${result}: ${output} ${error}")
endif()
