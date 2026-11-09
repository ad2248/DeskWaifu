find_package(Qt6 6.5 REQUIRED COMPONENTS Multimedia Network DBus)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SAMPLERATE REQUIRED IMPORTED_TARGET samplerate)

add_library(deskwaifu_voice_input STATIC
  VoiceInputSettings.cpp
  private/VoiceInputController.cpp
  private/VoiceInputState.cpp
  private/audio/AudioCapture.cpp
  private/audio/PcmNormalizer.cpp
  private/audio/WavEncoder.cpp
  private/asr/NativeAsrClient.cpp)
set_target_properties(deskwaifu_voice_input PROPERTIES AUTOMOC ON)
target_include_directories(deskwaifu_voice_input PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/private")
target_link_libraries(deskwaifu_voice_input PUBLIC
  deskwaifu_plugin_sdk Qt6::Multimedia Qt6::Network PkgConfig::SAMPLERATE)

deskwaifu_add_plugin(voice_input VoiceInputPlugin.cpp)
target_link_libraries(voice_input_plugin PRIVATE deskwaifu_voice_input Qt6::DBus)

if(DESKWAIFU_BUILD_CONTROL)
  find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets)
  target_sources(voice_input_plugin PRIVATE
    gui/VoiceInputGuiPlugin.cpp gui/VoiceInputTab.cpp gui/VoiceInputTab.hpp)
  target_include_directories(voice_input_plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/gui")
  target_link_libraries(voice_input_plugin PRIVATE deskwaifu_gui_plugin_sdk)
  set_target_properties(voice_input_plugin PROPERTIES AUTOMOC ON)
endif()

if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  deskwaifu_add_test(NAME voice-input-smoke-mock COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/test_smoke_mock.py)
  set_tests_properties(voice-input-smoke-mock PROPERTIES LABELS "contract;plugin")
  # Private integration fixture; the production module never enables HTTP injection.
  add_library(voice-input-smoke-fixture MODULE tests/VoiceInputSmokePlugin.cpp)
  target_link_libraries(voice-input-smoke-fixture PRIVATE deskwaifu_voice_input Qt6::DBus)
  set_target_properties(voice-input-smoke-fixture PROPERTIES PREFIX "" OUTPUT_NAME voice_input_smoke
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/voice_input")
  # Explicit live entry point only: never discovered by the ordinary test gate.
  add_executable(voice-input-live-test tests/LiveAsrTest.cpp)
  target_link_libraries(voice-input-live-test PRIVATE deskwaifu_voice_input)
  find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
  foreach(component Audio Asr Settings State Controller)
    string(TOLOWER "${component}" name)
    add_executable(voice-input-${name}-test tests/${component}Test.cpp)
    target_link_libraries(voice-input-${name}-test PRIVATE deskwaifu_voice_input Qt6::Test)
    set_target_properties(voice-input-${name}-test PROPERTIES AUTOMOC ON)
    deskwaifu_add_test(NAME voice-input-${name} COMMAND voice-input-${name}-test)
    set_tests_properties(voice-input-${name} PROPERTIES LABELS "contract;plugin")
  endforeach()
  if(DESKWAIFU_BUILD_CONTROL)
    add_executable(voice-input-gui-test tests/GuiTest.cpp gui/VoiceInputTab.cpp gui/VoiceInputTab.hpp)
    target_include_directories(voice-input-gui-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/gui")
    target_link_libraries(voice-input-gui-test PRIVATE
      deskwaifu_voice_input deskwaifu_gui_plugin_sdk Qt6::Test Qt6::Widgets)
    set_target_properties(voice-input-gui-test PROPERTIES AUTOMOC ON)
    deskwaifu_add_test(NAME voice-input-gui COMMAND voice-input-gui-test)
    set_tests_properties(voice-input-gui PROPERTIES LABELS "contract;plugin;gui")
  endif()
endif()
