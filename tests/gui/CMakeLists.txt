cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(DeskWaifuGuiTabsTests LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 14)
  enable_testing()
endif()
find_package(Qt6 REQUIRED COMPONENTS Widgets Test Network)
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(PLUGIN_ROOT "${ROOT}/src/plugins")
add_executable(gui_tabs_test
    GuiTabsTest.cpp
    ${PLUGIN_ROOT}/system/gui/SystemTab.cpp ${PLUGIN_ROOT}/system/gui/SystemTab.hpp ${PLUGIN_ROOT}/system/gui/SystemTab.ui
    ${PLUGIN_ROOT}/interaction/gui/InteractionTab.cpp ${PLUGIN_ROOT}/interaction/gui/InteractionTab.hpp ${PLUGIN_ROOT}/interaction/gui/InteractionTab.ui
    ${PLUGIN_ROOT}/model/gui/ModelTab.cpp ${PLUGIN_ROOT}/model/gui/ModelTab.hpp ${PLUGIN_ROOT}/model/gui/ModelTab.ui
    ${PLUGIN_ROOT}/log/gui/LogTab.cpp ${PLUGIN_ROOT}/log/gui/LogTab.hpp ${PLUGIN_ROOT}/log/gui/LogTab.ui
    ${PLUGIN_ROOT}/speech/gui/SpeechTab.cpp ${PLUGIN_ROOT}/speech/gui/SpeechTab.hpp ${PLUGIN_ROOT}/speech/gui/SpeechTab.ui
    ${PLUGIN_ROOT}/system/gui/DeskWaifuLauncher.cpp
    ${PLUGIN_ROOT}/system/gui/DeskWaifuRespawner.cpp ${PLUGIN_ROOT}/system/gui/DeskWaifuRespawner.hpp
    ${PLUGIN_ROOT}/interaction/gui/HotkeyPanel.cpp ${PLUGIN_ROOT}/interaction/gui/HotkeyPanel.hpp)
set_target_properties(gui_tabs_test PROPERTIES AUTOMOC ON AUTOUIC ON)
if(TARGET deskwaifu_plugin_sdk)
  target_link_libraries(gui_tabs_test PRIVATE deskwaifu_plugin_sdk)
else()
  target_sources(gui_tabs_test PRIVATE ${ROOT}/src/plugin/GuiHostServices.hpp)
endif()
target_include_directories(gui_tabs_test PRIVATE ${ROOT}/src)
foreach(domain system interaction model log speech)
  target_include_directories(gui_tabs_test PRIVATE "${PLUGIN_ROOT}/${domain}/gui")
endforeach()
target_link_libraries(gui_tabs_test PRIVATE Qt6::Widgets Qt6::Test Qt6::Network)
add_test(NAME gui-tabs COMMAND gui_tabs_test)
set_tests_properties(gui-tabs PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
if(TARGET deskwaifu-control)
  add_executable(control-smoke-test ControlSmokeTest.cpp)
  target_link_libraries(control-smoke-test PRIVATE Qt6::Test Qt6::Network)
  target_compile_definitions(control-smoke-test PRIVATE CONTROL_BINARY="$<TARGET_FILE:deskwaifu-control>")
  set_target_properties(control-smoke-test PROPERTIES AUTOMOC ON)
  add_dependencies(control-smoke-test deskwaifu-control)
  add_test(NAME control-smoke-probe COMMAND control-smoke-test)
  set_tests_properties(control-smoke-probe PROPERTIES TIMEOUT 30)
endif()
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
    string(TOLOWER "${tab}" domain)
    add_library(${domain}_gui_entry OBJECT "${PLUGIN_ROOT}/${domain}/gui/${tab}GuiPlugin.cpp")
    target_include_directories(${domain}_gui_entry PRIVATE ${ROOT}/src)
    target_link_libraries(${domain}_gui_entry PRIVATE Qt6::Widgets)
endforeach()
endif()
