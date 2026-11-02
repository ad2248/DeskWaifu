cmake_minimum_required(VERSION 3.25)

# 无需安装旧版 LayerShellQt，即可测试应用真实依赖要求。
# 屏幕选择使用的 API 要求 6.6 版本。
file(READ "${APP_SOURCE}/CMakeLists.txt" source)
string(REGEX MATCH "find_package\\(LayerShellQt[^\n]*\\)" requirement "${source}")
if (NOT requirement)
  message(FATAL_ERROR "Missing LayerShellQt dependency requirement")
endif ()
string(REPLACE "REQUIRED)" "REQUIRED CONFIG NO_DEFAULT_PATH PATHS \"${FIXTURE_DIR}/package\")"
  requirement "${requirement}")
file(MAKE_DIRECTORY "${FIXTURE_DIR}/package")
file(WRITE "${FIXTURE_DIR}/package/LayerShellQtConfig.cmake" "")
file(WRITE "${FIXTURE_DIR}/CMakeLists.txt"
  "cmake_minimum_required(VERSION 3.25)\nproject(LayerShellVersion NONE)\n${requirement}\n")
foreach(version 6.5 6.6)
  file(WRITE "${FIXTURE_DIR}/package/LayerShellQtConfigVersion.cmake"
    "set(PACKAGE_VERSION ${version})\nif(PACKAGE_FIND_VERSION VERSION_GREATER PACKAGE_VERSION)\nset(PACKAGE_VERSION_COMPATIBLE FALSE)\nelse()\nset(PACKAGE_VERSION_COMPATIBLE TRUE)\nendif()\n")
  execute_process(COMMAND "${CMAKE_COMMAND}" -S "${FIXTURE_DIR}" -B "${FIXTURE_DIR}/build-${version}"
    "-DLayerShellQt_DIR=${FIXTURE_DIR}/package"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if (version STREQUAL "6.5" AND result EQUAL 0)
    message(FATAL_ERROR "LayerShellQt 6.5 must be rejected before compiling unavailable APIs")
  elseif (version STREQUAL "6.6" AND NOT result EQUAL 0)
    message(FATAL_ERROR "LayerShellQt 6.6 must be accepted: ${output}${error}")
  endif ()
endforeach()
