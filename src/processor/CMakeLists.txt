find_package(Qt6 6.5 REQUIRED COMPONENTS Gui Concurrent)
add_subdirectory(screenshot)
if(DESKWAIFU_BUILD_SPEECH_PLUGIN)
  add_subdirectory(speech)
endif()

add_library(deskwaifu_events SHARED
  ../event/ProcessorEvent.hpp ../event/ProcessorEventLoop.hpp ../event/ProcessorEventLoop.cpp)
target_include_directories(deskwaifu_events PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/deskwaifu>)
target_link_libraries(deskwaifu_events PUBLIC Qt6::Core)
set_target_properties(deskwaifu_events PROPERTIES VERSION 1.0 SOVERSION 1)
add_subdirectory(../plugin ${CMAKE_CURRENT_BINARY_DIR}/plugin)

add_library(deskwaifu_processors STATIC
  Processor.hpp Events.hpp ProcessorRuntime.hpp interaction/InteractionProcessor.cpp input/InputListener.cpp
  interaction/InteractionContext.cpp interaction/AppCommandHandler.cpp
  interaction/config/LAppConfig.cpp ../contracts/ConfigStore.cpp
  image/ImageFile.cpp image/ImageProcessor.cpp
  ocr/OcrProcessor.cpp screenshot/ScreenshotProcessor.cpp
  ipc/IpcProcessor.cpp signal/SignalProcessor.cpp)
target_include_directories(deskwaifu_processors PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_definitions(deskwaifu_processors PRIVATE
  DESKWAIFU_PLUGIN_DIR="${CMAKE_INSTALL_FULL_LIBDIR}/deskwaifu/plugins")
target_link_libraries(deskwaifu_processors PUBLIC deskwaifu_events deskwaifu_ocr_client
  deskwaifu_screenshot deskwaifu_ipc deskwaifu_input deskwaifu_output deskwaifu_compositor deskwaifu_offscreen deskwaifu_window_state deskwaifu_kwin_cursor Qt6::Gui Qt6::Concurrent)
add_library(deskwaifu_reading STATIC reading_clip/ReadingClipProcessor.cpp)
set_target_properties(deskwaifu_reading PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(deskwaifu_reading PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(deskwaifu_reading PUBLIC deskwaifu_events Qt6::Gui)
add_subdirectory(../plugins ${CMAKE_CURRENT_BINARY_DIR}/plugins)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
