add_subdirectory(private/processor/speech)
deskwaifu_add_plugin(speech SpeechPlugin.cpp)
target_link_libraries(speech_plugin PRIVATE deskwaifu_speech)

if(DESKWAIFU_BUILD_CONTROL)
  target_sources(speech_plugin PRIVATE
    gui/SpeechGuiPlugin.cpp gui/SpeechTab.cpp
    gui/SpeechTab.hpp gui/SpeechTab.ui)
  target_include_directories(speech_plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/gui")
  target_link_libraries(speech_plugin PRIVATE deskwaifu_gui_plugin_sdk)
  set_target_properties(speech_plugin PROPERTIES AUTOMOC ON AUTOUIC ON
    AUTOUIC_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/gui")
endif()

if(BUILD_TESTING)
  add_executable(speech-endpoint-test tests/SpeechEndpointTest.cpp)
  target_link_libraries(speech-endpoint-test PRIVATE deskwaifu_speech)
  add_test(NAME speech-endpoint COMMAND speech-endpoint-test)
  set_tests_properties(speech-endpoint PROPERTIES TIMEOUT 30 LABELS "contract;plugin")
endif()
