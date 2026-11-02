# Shared fixtures are test-only: domain tests never acquire another plugin's includes.
add_library(deskwaifu_test_support INTERFACE)
target_include_directories(deskwaifu_test_support INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(deskwaifu_test_support INTERFACE deskwaifu_plugin_sdk)
add_library(deskwaifu_backend_test_support STATIC LoadedBackendPlugins.cpp)
target_link_libraries(deskwaifu_backend_test_support PUBLIC deskwaifu_test_support PRIVATE deskwaifu_plugin_host)

function(deskwaifu_domain_test target owner)
  if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/src/plugins/${owner}/private")
    target_include_directories(${target} PRIVATE
      "${CMAKE_SOURCE_DIR}/src/plugins/${owner}/private")
  endif()
  target_link_libraries(${target} PRIVATE deskwaifu_plugin_sdk)
endfunction()

if(DESKWAIFU_BUILD_CONTROL)
  if(TARGET ipc_plugin)
  add_library(deskwaifu_gui_test_support STATIC LoadedGuiTransport.cpp)
  target_link_libraries(deskwaifu_gui_test_support PUBLIC deskwaifu_test_support deskwaifu_gui_plugin_sdk
    PRIVATE deskwaifu_gui_plugin_host)
  target_compile_definitions(deskwaifu_gui_test_support PRIVATE IPC_PLUGIN_FILE="$<TARGET_FILE:ipc_plugin>")
  add_dependencies(deskwaifu_gui_test_support ipc_plugin)
  endif()
  qt_add_executable(spawn-backend-fixture SpawnBackendFixture.cpp)
  target_link_libraries(spawn-backend-fixture PRIVATE Qt6::Core)
  set_target_properties(spawn-backend-fixture PROPERTIES
    OUTPUT_NAME deskwaifu
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/spawn-fixture")
endif()
