# 普通测试逐项隔离，直接运行 CTest 时也不借用宿主桌面与用户目录。
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(DBUS_RUN_SESSION dbus-run-session REQUIRED)

function(deskwaifu_add_test)
  cmake_parse_arguments(PARSE_ARGV 0 TEST "" "NAME" "COMMAND;ENVIRONMENT")
  list(POP_FRONT TEST_COMMAND executable)
  if(TARGET "${executable}")
    set(executable "$<TARGET_FILE:${executable}>")
  endif()
  add_test(NAME "${TEST_NAME}" COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../scripts/test-environment.py"
    "${CMAKE_COMMAND}" -E env ${TEST_ENVIRONMENT} "${executable}" ${TEST_COMMAND})
endfunction()
