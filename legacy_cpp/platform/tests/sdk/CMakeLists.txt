get_target_property(sdk_source_directory deskwaifu_plugin_sdk SOURCE_DIR)
get_target_property(sdk_binary_directory deskwaifu_plugin_sdk BINARY_DIR)
deskwaifu_add_test(NAME plugin-sdk-ownership COMMAND ${CMAKE_COMMAND}
  -DOWNERSHIP_SCRIPT=${sdk_source_directory}/cmake/CheckOwnership.cmake
  -P ${CMAKE_CURRENT_SOURCE_DIR}/OwnershipTest.cmake)
deskwaifu_add_test(NAME plugin-sdk-installed-consumer COMMAND ${CMAKE_COMMAND}
  -DSDK_BINARY_DIRECTORY=${sdk_binary_directory}
  -DCONSUMER_SOURCE_DIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}/consumer
  -DSDK_CXX_COMPILER=${CMAKE_CXX_COMPILER}
  -DTEST_GUI=${DESKWAIFU_BUILD_GUI_PLUGIN_SDK}
  -P ${CMAKE_CURRENT_SOURCE_DIR}/InstalledConsumerTest.cmake)
