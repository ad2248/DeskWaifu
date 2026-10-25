find_package(Qt6 6.5 REQUIRED COMPONENTS Gui Concurrent)
add_subdirectory(screenshot)
add_subdirectory(speech)

add_library(deskwaifu_events STATIC
  ../event/ProcessorEvent.hpp ../event/ProcessorEventLoop.hpp ../event/ProcessorEventLoop.cpp)
target_include_directories(deskwaifu_events PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(deskwaifu_events PUBLIC Qt6::Core)

add_library(deskwaifu_processors STATIC
  Processor.hpp Events.hpp ProcessorRuntime.hpp interaction/InteractionProcessor.cpp input/InputListener.cpp
  interaction/InteractionContext.cpp interaction/AppCommandHandler.cpp
  interaction/config/LAppConfig.cpp ../contracts/ConfigStore.cpp
  image/ImageFile.cpp image/ImageProcessor.cpp
  ocr/OcrProcessor.cpp screenshot/ScreenshotProcessor.cpp
  speech/SpeechProcessor.cpp reading_clip/ReadingClipProcessor.cpp ipc/IpcProcessor.cpp signal/SignalProcessor.cpp)
target_include_directories(deskwaifu_processors PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(deskwaifu_processors PUBLIC deskwaifu_events deskwaifu_ocr_client
  deskwaifu_screenshot deskwaifu_speech deskwaifu_ipc deskwaifu_input deskwaifu_output deskwaifu_compositor deskwaifu_offscreen deskwaifu_window_state deskwaifu_kwin_cursor Qt6::Gui Qt6::Concurrent)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
