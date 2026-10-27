find_package(Threads REQUIRED)
get_target_property(sdk_source_directory deskwaifu_plugin_sdk SOURCE_DIR)
get_target_property(sdk_binary_directory deskwaifu_plugin_sdk BINARY_DIR)

add_executable(plugin-event-test EventTest.cpp)
target_link_libraries(plugin-event-test PRIVATE deskwaifu_plugin_sdk Threads::Threads)
add_test(NAME plugin-events COMMAND plugin-event-test)
add_executable(plugin-timeout-flood-test TimeoutFloodTest.cpp)
target_link_libraries(plugin-timeout-flood-test PRIVATE deskwaifu_plugin_sdk)
add_test(NAME plugin-timeout-flood COMMAND plugin-timeout-flood-test)
set_tests_properties(plugin-timeout-flood PROPERTIES TIMEOUT 5)

add_executable(plugin-legacy-event-test ProcessorEventLoopTest.cpp)
target_link_libraries(plugin-legacy-event-test PRIVATE deskwaifu_events Threads::Threads)
add_test(NAME plugin-legacy-events COMMAND plugin-legacy-event-test)
add_executable(plugin-sdk-contract-test PluginContractTest.cpp)
target_link_libraries(plugin-sdk-contract-test PRIVATE deskwaifu_plugin_sdk)
if(DESKWAIFU_BUILD_GUI_PLUGIN_SDK)
  target_link_libraries(plugin-sdk-contract-test PRIVATE deskwaifu_gui_plugin_sdk)
  target_compile_definitions(plugin-sdk-contract-test PRIVATE DESKWAIFU_TEST_GUI)
endif()
add_test(NAME plugin-sdk-contracts COMMAND plugin-sdk-contract-test)
add_test(NAME plugin-sdk-ownership COMMAND ${CMAKE_COMMAND}
  -DOWNERSHIP_SCRIPT=${sdk_source_directory}/cmake/CheckOwnership.cmake
  -P ${CMAKE_CURRENT_SOURCE_DIR}/OwnershipTest.cmake)
add_test(NAME plugin-sdk-installed-consumer COMMAND ${CMAKE_COMMAND}
  -DSDK_BINARY_DIRECTORY=${sdk_binary_directory}
  -DCONSUMER_SOURCE_DIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}/consumer
  -DSDK_CXX_COMPILER=${CMAKE_CXX_COMPILER}
  -DTEST_GUI=${DESKWAIFU_BUILD_GUI_PLUGIN_SDK}
  -P ${CMAKE_CURRENT_SOURCE_DIR}/InstalledConsumerTest.cmake)
set_tests_properties(plugin-sdk-installed-consumer PROPERTIES TIMEOUT 60)
set_tests_properties(plugin-events plugin-legacy-events plugin-sdk-contracts PROPERTIES TIMEOUT 30)
