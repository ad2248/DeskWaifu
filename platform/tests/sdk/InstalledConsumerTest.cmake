set(fixture "${CMAKE_CURRENT_BINARY_DIR}/sdk-package-consumer")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${SDK_BINARY_DIRECTORY}" --prefix "${fixture}/prefix"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "SDK installation failed: ${output}\n${error}")
endif()
if(EXISTS "${fixture}/prefix/include/deskwaifu/plugin/PluginManager.hpp" OR
   EXISTS "${fixture}/prefix/include/deskwaifu/plugin/GuiPluginManager.hpp")
  message(FATAL_ERROR "The SDK must not install platform plugin loaders")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${CONSUMER_SOURCE_DIRECTORY}" -B "${fixture}/build"
  -DCMAKE_PREFIX_PATH=${fixture}/prefix -DCMAKE_CXX_COMPILER=${SDK_CXX_COMPILER}
  -DCMAKE_CXX_STANDARD=14 -DTEST_GUI=${TEST_GUI}
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Installed SDK consumer configuration failed: ${output}\n${error}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${fixture}/build" --parallel 2
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Installed SDK consumer build failed: ${output}\n${error}")
endif()
execute_process(COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${fixture}/build" --output-on-failure
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Installed SDK consumer tests failed: ${output}\n${error}")
endif()
