function(deskwaifu_add_plugin name)
  add_library(${name}_plugin MODULE ${ARGN})
  target_link_libraries(${name}_plugin PRIVATE deskwaifu_plugin_sdk)
  set_target_properties(${name}_plugin PROPERTIES PREFIX "" OUTPUT_NAME "${name}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/${name}"
    INSTALL_RPATH "$ORIGIN/../../..")
  install(TARGETS ${name}_plugin LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/deskwaifu/plugins/${name}
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
