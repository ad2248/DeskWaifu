add_library(deskwaifu_ipc STATIC
  private/processor/ipc/LAppIPC.cpp
  private/processor/ipc/LAppIPC.hpp
  private/processor/ipc/IpcSocketPath.hpp
)
target_include_directories(deskwaifu_ipc PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/private")
target_link_libraries(deskwaifu_ipc PUBLIC deskwaifu_plugin_sdk)

add_library(deskwaifu_ipc_core STATIC
  private/processor/command/CommandProcessor.hpp
  private/processor/ipc/IpcProcessor.cpp
  private/processor/ipc/IpcProcessor.hpp
)
target_include_directories(deskwaifu_ipc_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/private")
target_link_libraries(deskwaifu_ipc_core PUBLIC deskwaifu_ipc)
set_target_properties(deskwaifu_ipc deskwaifu_ipc_core PROPERTIES
  POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

deskwaifu_add_plugin(ipc IpcPlugin.cpp)
target_include_directories(ipc_plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/private")
target_link_libraries(ipc_plugin PRIVATE deskwaifu_ipc_core)
install(PROGRAMS "${PROJECT_SOURCE_DIR}/deskwaifu-ctl"
  DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DeskWaifu)

if(DESKWAIFU_BUILD_CONTROL)
  find_package(Qt6 REQUIRED COMPONENTS Network)
  target_sources(ipc_plugin PRIVATE
    gui/IpcGuiPlugin.cpp gui/DeskWaifuClient.cpp gui/DeskWaifuClient.hpp)
  target_include_directories(ipc_plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/gui")
  target_link_libraries(ipc_plugin PRIVATE deskwaifu_gui_plugin_sdk Qt6::Network)
  set_target_properties(ipc_plugin PROPERTIES AUTOMOC ON)
endif()

if(BUILD_TESTING)
  add_executable(ipc-command-gateway-test tests/CommandGatewayTest.cpp)
  target_include_directories(ipc-command-gateway-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/private")
  target_link_libraries(ipc-command-gateway-test PRIVATE deskwaifu_plugin_sdk)
  add_test(NAME ipc-command-gateway COMMAND ipc-command-gateway-test)
  set_tests_properties(ipc-command-gateway PROPERTIES TIMEOUT 30 LABELS "contract;plugin;ipc")
  add_executable(ipc-context-lifecycle-test tests/ContextLifecycleTest.cpp)
  target_include_directories(ipc-context-lifecycle-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/private")
  target_link_libraries(ipc-context-lifecycle-test PRIVATE deskwaifu_ipc_core)
  add_test(NAME ipc-context-lifecycle COMMAND ipc-context-lifecycle-test)
  set_tests_properties(ipc-context-lifecycle PROPERTIES TIMEOUT 30 LABELS "contract;plugin;ipc;lifecycle")
endif()
