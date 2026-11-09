cmake_minimum_required(VERSION 3.25)

set(fixture "${CMAKE_CURRENT_BINARY_DIR}/sdk-package-consumer")
# A previous installation may contain headers that no longer belong to the SDK.
file(REMOVE_RECURSE "${fixture}/prefix")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${SDK_BINARY_DIRECTORY}" --prefix "${fixture}/prefix"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "SDK installation failed: ${output}\n${error}")
endif()
foreach(header plugin/PluginManager.hpp plugin/GuiPluginManager.hpp
    contracts/ModelInputRegion.hpp contracts/ModelHitTest.hpp
    contracts/RenderFrame.hpp contracts/ModelExchange.hpp)
  if(EXISTS "${fixture}/prefix/include/deskwaifu/${header}")
    message(FATAL_ERROR "The SDK must not install private platform or desktop headers: ${header}")
  endif()
endforeach()
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
