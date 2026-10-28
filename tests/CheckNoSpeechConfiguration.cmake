cmake_minimum_required(VERSION 3.25)

execute_process(COMMAND "${CMAKE_COMMAND}" -S "${SOURCE_DIR}" -B "${BINARY_DIR}" -G "${GENERATOR}"
  "-DSDK_ROOT_PATH=${SDK_ROOT}" "-DGLEW_PATH=${GLEW_ROOT}"
  -DBUILD_TESTING=ON -DDESKWAIFU_BUILD_CONTROL=OFF -DDESKWAIFU_BUILD_SPEECH_PLUGIN=OFF
  -DDESKWAIFU_TEST_GATE=OFF
  -DCMAKE_DISABLE_FIND_PACKAGE_Qt6WebSockets=TRUE -DCMAKE_DISABLE_FIND_PACKAGE_Qt6Multimedia=TRUE
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Disabling speech must not require its dependencies, even with tests enabled:\n${output}\n${error}")
endif()
