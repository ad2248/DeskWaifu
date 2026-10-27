function(deskwaifu_add_plugin name)
  cmake_parse_arguments(PLUGIN "GUI" "" "" ${ARGN})
  if(PLUGIN_GUI)
    set(target ${name}_gui_plugin)
    add_library(${target} MODULE ${PLUGIN_UNPARSED_ARGUMENTS})
    target_link_libraries(${target} PRIVATE deskwaifu_gui_plugin_sdk)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/${name}/gui")
    set_target_properties(${target} PROPERTIES AUTOMOC ON AUTOUIC ON)
  else()
    set(target ${name}_plugin)
    add_library(${target} MODULE ${PLUGIN_UNPARSED_ARGUMENTS})
    target_link_libraries(${target} PRIVATE deskwaifu_plugin_sdk)
  endif()
  set_target_properties(${target} PROPERTIES PREFIX "" OUTPUT_NAME "${name}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/${name}"
    INSTALL_RPATH "$ORIGIN/../../..")
  install(TARGETS ${target} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/deskwaifu/plugins/${name}
    COMPONENT DeskWaifu)
endfunction()

deskwaifu_add_plugin(reading_clip reading_clip/ReadingPlugin.cpp)
target_link_libraries(reading_clip_plugin PRIVATE deskwaifu_reading)

if(DESKWAIFU_BUILD_SPEECH_PLUGIN)
  deskwaifu_add_plugin(speech speech/SpeechPlugin.cpp ../contracts/ConfigStore.cpp)
  target_link_libraries(speech_plugin PRIVATE deskwaifu_speech)
  if(BUILD_TESTING)
    add_executable(speech-plugin-load-test speech/tests/SpeechPluginLoadTest.cpp)
    target_link_libraries(speech-plugin-load-test PRIVATE deskwaifu_plugin_host)
    add_test(NAME speech-plugin-load COMMAND speech-plugin-load-test $<TARGET_FILE:speech_plugin>)
    add_executable(speech-endpoint-test speech/tests/SpeechEndpointTest.cpp)
    target_link_libraries(speech-endpoint-test PRIVATE deskwaifu_speech)
    add_test(NAME speech-endpoint COMMAND speech-endpoint-test)
    set_tests_properties(speech-plugin-load speech-endpoint PROPERTIES TIMEOUT 30 LABELS "contract;plugin")
  endif()
endif()

if(DESKWAIFU_BUILD_CONTROL)
  deskwaifu_add_plugin(system GUI
    system/gui/SystemGuiPlugin.cpp system/gui/SystemTab.cpp
    system/gui/SystemTab.hpp system/gui/SystemTab.ui
    system/gui/DeskWaifuLauncher.cpp system/gui/DeskWaifuLauncher.hpp
    system/gui/DeskWaifuRespawner.cpp system/gui/DeskWaifuRespawner.hpp)
  deskwaifu_add_plugin(interaction GUI
    interaction/gui/InteractionGuiPlugin.cpp interaction/gui/InteractionTab.cpp
    interaction/gui/InteractionTab.hpp interaction/gui/InteractionTab.ui
    interaction/gui/HotkeyPanel.cpp interaction/gui/HotkeyPanel.hpp)
  deskwaifu_add_plugin(model GUI
    model/gui/ModelGuiPlugin.cpp model/gui/ModelTab.cpp
    model/gui/ModelTab.hpp model/gui/ModelTab.ui)
  deskwaifu_add_plugin(log GUI
    log/gui/LogGuiPlugin.cpp log/gui/LogTab.cpp
    log/gui/LogTab.hpp log/gui/LogTab.ui)
  add_custom_target(deskwaifu_control_plugins DEPENDS
    system_gui_plugin interaction_gui_plugin model_gui_plugin log_gui_plugin)
  if(DESKWAIFU_BUILD_SPEECH_PLUGIN)
    target_sources(speech_plugin PRIVATE
      speech/gui/SpeechGuiPlugin.cpp speech/gui/SpeechTab.cpp
      speech/gui/SpeechTab.hpp speech/gui/SpeechTab.ui)
    target_include_directories(speech_plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/speech/gui")
    target_link_libraries(speech_plugin PRIVATE deskwaifu_gui_plugin_sdk)
    set_target_properties(speech_plugin PROPERTIES AUTOMOC ON AUTOUIC ON
      AUTOUIC_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/speech/gui")
    add_dependencies(deskwaifu_control_plugins speech_plugin)
  endif()
endif()
