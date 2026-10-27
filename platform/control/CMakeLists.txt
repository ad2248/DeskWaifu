find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_library(deskwaifu_control_shell STATIC
  MainWindow.cpp
  MainWindow.hpp
  MainWindow.ui
  ControlHostServices.cpp
  ControlHostServices.hpp
)

target_link_libraries(deskwaifu_control_shell PUBLIC
  Qt6::Widgets
  deskwaifu_gui_plugin_sdk
  deskwaifu_gui_plugin_host
)
target_include_directories(deskwaifu_control_shell PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(deskwaifu_control_shell PRIVATE
  DESKWAIFU_PLUGIN_DIR="${CMAKE_INSTALL_FULL_LIBDIR}/deskwaifu/plugins")

set_target_properties(deskwaifu_control_shell PROPERTIES
  AUTOUIC ON
  AUTOMOC ON
)

qt_add_executable(deskwaifu-control main.cpp)
target_link_libraries(deskwaifu-control PRIVATE deskwaifu_control_shell)
set_target_properties(deskwaifu-control PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")

install(TARGETS deskwaifu-control
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DeskWaifu)
install(FILES deskwaifu-control.desktop
  DESTINATION ${CMAKE_INSTALL_DATADIR}/applications COMPONENT DeskWaifu)

if(BUILD_TESTING)
  add_executable(gui-transport-broker-test tests/GuiTransportBrokerTest.cpp)
  target_link_libraries(gui-transport-broker-test PRIVATE deskwaifu_control_shell)
  add_test(NAME gui-transport-broker COMMAND gui-transport-broker-test)
  set_tests_properties(gui-transport-broker PROPERTIES TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "contract;gui")
endif()
