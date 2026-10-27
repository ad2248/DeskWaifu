find_package(Qt6 6.5 REQUIRED COMPONENTS Gui Concurrent)
add_subdirectory(private/processor/screenshot)
add_library(deskwaifu_ocr_client STATIC private/processor/ocr/OcrClient.cpp private/processor/ocr/OcrClient.hpp)
target_include_directories(deskwaifu_ocr_client PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/private/processor/ocr")
target_link_libraries(deskwaifu_ocr_client PUBLIC Qt6::Core)
target_compile_definitions(deskwaifu_ocr_client PRIVATE
  DESKWAIFU_OCR_HELPER_PATH="${CMAKE_INSTALL_FULL_LIBEXECDIR}/deskwaifu-ocr")
set_target_properties(deskwaifu_ocr_client PROPERTIES AUTOMOC ON)
add_library(deskwaifu_perception STATIC
  private/processor/image/ImageFile.cpp private/processor/image/ImageProcessor.cpp
  private/processor/ocr/OcrProcessor.cpp private/processor/screenshot/ScreenshotProcessor.cpp)
target_include_directories(deskwaifu_perception PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/private")
target_link_libraries(deskwaifu_perception PUBLIC deskwaifu_plugin_sdk deskwaifu_ocr_client deskwaifu_screenshot Qt6::Gui Qt6::Concurrent)
deskwaifu_add_plugin(perception PerceptionPlugin.cpp)
target_link_libraries(perception_plugin PRIVATE deskwaifu_perception)
