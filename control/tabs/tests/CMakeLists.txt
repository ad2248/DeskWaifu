cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(DeskWaifuGuiTabsTests LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 14)
  enable_testing()
endif()
find_package(Qt6 REQUIRED COMPONENTS Widgets Test Network)
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
add_executable(gui_tabs_test
    GuiTabsTest.cpp
    ../SystemTab.cpp ../SystemTab.hpp ../SystemTab.ui
    ../InteractionTab.cpp ../InteractionTab.hpp ../InteractionTab.ui
    ../ModelTab.cpp ../ModelTab.hpp ../ModelTab.ui
    ../LogTab.cpp ../LogTab.hpp ../LogTab.ui
    ../SpeechTab.cpp ../SpeechTab.hpp ../SpeechTab.ui
    ${ROOT}/control/DeskWaifuLauncher.cpp
    ${ROOT}/control/DeskWaifuRespawner.cpp ${ROOT}/control/DeskWaifuRespawner.hpp
    ${ROOT}/control/HotkeyPanel.cpp ${ROOT}/control/HotkeyPanel.hpp)
set_target_properties(gui_tabs_test PROPERTIES AUTOMOC ON AUTOUIC ON)
if(TARGET deskwaifu_plugin_sdk)
  target_link_libraries(gui_tabs_test PRIVATE deskwaifu_plugin_sdk)
else()
  target_sources(gui_tabs_test PRIVATE ${ROOT}/src/plugin/GuiHostServices.hpp)
endif()
target_include_directories(gui_tabs_test PRIVATE .. ${ROOT}/control ${ROOT}/src)
target_link_libraries(gui_tabs_test PRIVATE Qt6::Widgets Qt6::Test Qt6::Network)
add_test(NAME gui-tabs COMMAND gui_tabs_test)
set_tests_properties(gui-tabs PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
if(TARGET system_gui_plugin)
  add_library(gui_shutdown_pump MODULE ShutdownPump.cpp)
  target_link_libraries(gui_shutdown_pump PRIVATE deskwaifu_gui_plugin_sdk)
  add_executable(gui-shutdown-test GuiShutdownTest.cpp)
  target_link_libraries(gui-shutdown-test PRIVATE deskwaifu_gui_plugin_sdk)
  set_target_properties(gui-shutdown-test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/shutdown-fixture-bin")
  add_dependencies(gui-shutdown-test system_gui_plugin gui_shutdown_pump)
  add_test(NAME gui-plugin-shutdown COMMAND gui-shutdown-test
    $<TARGET_FILE:system_gui_plugin> $<TARGET_FILE:gui_shutdown_pump>)
  set_tests_properties(gui-plugin-shutdown PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 10 RUN_SERIAL TRUE)
endif()
# Compile each DSO entry independently; they intentionally export the same symbol.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
foreach(tab System Interaction Model Log Speech)
    add_library(${tab}_gui_entry OBJECT ../${tab}GuiPlugin.cpp)
    target_include_directories(${tab}_gui_entry PRIVATE .. ${ROOT}/src)
    target_link_libraries(${tab}_gui_entry PRIVATE Qt6::Widgets)
endforeach()
endif()
