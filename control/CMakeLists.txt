find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

add_library(deskwaifu_control_shell STATIC
  MainWindow.cpp
  MainWindow.hpp
  MainWindow.ui
  DeskWaifuClient.cpp
  DeskWaifuClient.hpp
  ../src/processor/ipc/IpcSocketPath.hpp
  ControlHostServices.cpp
  ControlHostServices.hpp
)

target_link_libraries(deskwaifu_control_shell PUBLIC
  Qt6::Widgets
  Qt6::Network
  deskwaifu_gui_plugin_sdk
)
target_include_directories(deskwaifu_control_shell PUBLIC . ../src)
target_compile_definitions(deskwaifu_control_shell PRIVATE
  DESKWAIFU_PLUGIN_DIR="${CMAKE_INSTALL_FULL_LIBDIR}/deskwaifu/plugins")

set_target_properties(deskwaifu_control_shell PROPERTIES
  AUTOUIC ON
  AUTOMOC ON
)

qt_add_executable(deskwaifu-control main.cpp)
target_link_libraries(deskwaifu-control PRIVATE deskwaifu_control_shell)
set_target_properties(deskwaifu-control PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")

add_dependencies(deskwaifu-control deskwaifu_control_plugins)

install(TARGETS deskwaifu-control
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DeskWaifu)
install(FILES deskwaifu-control.desktop
  DESTINATION ${CMAKE_INSTALL_DATADIR}/applications COMPONENT DeskWaifu)
