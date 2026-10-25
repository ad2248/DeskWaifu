find_package(Qt6 6.5 REQUIRED COMPONENTS WebSockets Multimedia StateMachine)
add_library(deskwaifu_speech STATIC
  SpeechTypes.hpp SpeechPlayer.cpp SpeechPlayer.hpp
  audio/AudioPlayer.cpp audio/AudioPlayer.hpp
  source/MiniMaxSpeechSource.cpp source/MiniMaxSpeechSource.hpp)
set_target_properties(deskwaifu_speech PROPERTIES AUTOMOC ON)
target_include_directories(deskwaifu_speech PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(deskwaifu_speech PUBLIC Qt6::WebSockets Qt6::Multimedia Qt6::StateMachine)
