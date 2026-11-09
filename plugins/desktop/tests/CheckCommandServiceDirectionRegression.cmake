cmake_minimum_required(VERSION 3.25)

file(MAKE_DIRECTORY "${FIXTURE_SOURCE}/processor/interaction")
file(WRITE "${FIXTURE_SOURCE}/processor/interaction/AppCommandHandler.cpp" "// Commands only\n")
foreach(dependency "IExternalControlPort& port;" "LAppIPC* transport;" "auto port = app.GetIPC();")
  file(WRITE "${FIXTURE_SOURCE}/processor/interaction/AppCommandHandler.hpp" "${dependency}\n")
  execute_process(COMMAND "${CMAKE_COMMAND}" "-DAPP_SOURCE=${FIXTURE_SOURCE}"
    -P "${CMAKE_CURRENT_LIST_DIR}/CheckCommandServiceDirection.cmake"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if (result EQUAL 0 OR NOT error MATCHES "must not retain or access")
    message(FATAL_ERROR "Reverse dependency was not rejected: ${dependency}\n${output}${error}")
  endif ()
endforeach()
