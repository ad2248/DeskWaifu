cmake_minimum_required(VERSION 3.25)

set(fixture "${CMAKE_CURRENT_BINARY_DIR}/ownership-fixture")
file(MAKE_DIRECTORY "${fixture}/plugin")
foreach(header IN ITEMS "platform/plugin/PluginManager.hpp" "../private/Domain.hpp" "QLibrary" "QPluginLoader" "CubismFramework.hpp")
  file(WRITE "${fixture}/plugin/Forbidden.hpp" "#include \"${header}\"\n")
  execute_process(COMMAND "${CMAKE_COMMAND}" -DSDK_ROOT=${fixture} -P "${OWNERSHIP_SCRIPT}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(result EQUAL 0)
    message(FATAL_ERROR "SDK ownership guard accepted forbidden dependency: ${header}")
  endif()
endforeach()
file(WRITE "${fixture}/plugin/Forbidden.hpp" "#include <QString>\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -DSDK_ROOT=${fixture} -P "${OWNERSHIP_SCRIPT}"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "SDK ownership guard rejected a Qt Core dependency: ${error}")
endif()
