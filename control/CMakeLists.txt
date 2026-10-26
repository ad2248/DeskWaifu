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

function(deskwaifu_add_gui_tab name type)
  add_library(${name}_gui_plugin MODULE
    tabs/${type}GuiPlugin.cpp tabs/${type}Tab.cpp tabs/${type}Tab.hpp tabs/${type}Tab.ui
    ${ARGN})
  target_include_directories(${name}_gui_plugin PRIVATE . ../src)
  target_link_libraries(${name}_gui_plugin PRIVATE deskwaifu_gui_plugin_sdk)
  set_target_properties(${name}_gui_plugin PROPERTIES AUTOMOC ON AUTOUIC ON
    PREFIX "" OUTPUT_NAME "${name}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/${name}"
    INSTALL_RPATH "$ORIGIN/../../..")
  install(TARGETS ${name}_gui_plugin LIBRARY
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/deskwaifu/plugins/${name} COMPONENT DeskWaifu)
endfunction()

deskwaifu_add_gui_tab(system System DeskWaifuLauncher.cpp)
deskwaifu_add_gui_tab(interaction Interaction HotkeyPanel.cpp HotkeyPanel.hpp)
deskwaifu_add_gui_tab(model Model)
deskwaifu_add_gui_tab(log Log)
add_custom_target(deskwaifu_control_plugins DEPENDS
  system_gui_plugin interaction_gui_plugin model_gui_plugin log_gui_plugin)
if(DESKWAIFU_BUILD_SPEECH_PLUGIN)
  target_sources(speech_plugin PRIVATE
    tabs/SpeechGuiPlugin.cpp tabs/SpeechTab.cpp tabs/SpeechTab.hpp tabs/SpeechTab.ui)
  target_include_directories(speech_plugin PRIVATE .)
  target_link_libraries(speech_plugin PRIVATE deskwaifu_gui_plugin_sdk)
  set_target_properties(speech_plugin PROPERTIES AUTOMOC ON AUTOUIC ON
    AUTOUIC_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/tabs")
  add_dependencies(deskwaifu_control_plugins speech_plugin)
endif()
add_dependencies(deskwaifu-control deskwaifu_control_plugins)
if(BUILD_TESTING)
  add_subdirectory(tabs/tests)
endif()

install(TARGETS deskwaifu-control
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DeskWaifu)
install(FILES deskwaifu-control.desktop
  DESTINATION ${CMAKE_INSTALL_DATADIR}/applications COMPONENT DeskWaifu)
