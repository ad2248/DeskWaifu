add_library(deskwaifu_catalog STATIC processor/model/ModelCatalog.cpp)
target_include_directories(deskwaifu_catalog PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
add_library(deskwaifu_input STATIC InputController.cpp)
target_include_directories(deskwaifu_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
add_library(deskwaifu_output STATIC OutputController.cpp)
target_include_directories(deskwaifu_output PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
add_library(deskwaifu_compositor STATIC CompositorAdapter.cpp)
target_include_directories(deskwaifu_compositor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Threads REQUIRED)
target_link_libraries(deskwaifu_compositor PUBLIC Threads::Threads)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
add_library(deskwaifu_kwin_cursor STATIC KWinCursorTracker.cpp)
target_include_directories(deskwaifu_kwin_cursor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(deskwaifu_kwin_cursor PUBLIC PkgConfig::SYSTEMD)
add_library(deskwaifu_ipc STATIC LAppIPC.cpp)
target_include_directories(deskwaifu_ipc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
add_library(deskwaifu_loader STATIC processor/model/ModelLoader.cpp)
target_include_directories(deskwaifu_loader PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${COMMON_SRC_PATH})
target_link_libraries(deskwaifu_loader PUBLIC deskwaifu_catalog Framework)
add_library(deskwaifu_renderer STATIC processor/model/ModelRenderer.cpp)
target_include_directories(deskwaifu_renderer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${COMMON_SRC_PATH})
target_link_libraries(deskwaifu_renderer PUBLIC Framework)

  target_sources(${APP_NAME}
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/LAppConfig.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/ConfigStore.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/LAppConfig.hpp
      ${CMAKE_CURRENT_SOURCE_DIR}/LAppDefine.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/LAppDefine.hpp
      ${CMAKE_CURRENT_SOURCE_DIR}/LAppDelegate.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/LAppDelegate.hpp
      ${CMAKE_CURRENT_SOURCE_DIR}/AppCommandHandler.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/LAppIPC.hpp
      ${CMAKE_CURRENT_SOURCE_DIR}/processor/model/ModelFrameRegion.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/LAppLive2DManager.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/LAppLive2DManager.hpp
      ${CMAKE_CURRENT_SOURCE_DIR}/LAppModel.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/processor/model/ModelHotkeys.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/contracts/ModelTransform.hpp
      ${CMAKE_CURRENT_SOURCE_DIR}/LAppModel.hpp
      ${CMAKE_CURRENT_SOURCE_DIR}/LAppPal.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/LAppPal.hpp
      ${CMAKE_CURRENT_SOURCE_DIR}/LAppSprite.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/LAppSprite.hpp
      ${CMAKE_CURRENT_SOURCE_DIR}/LAppSpriteShader.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/LAppSpriteShader.hpp
      ${CMAKE_CURRENT_SOURCE_DIR}/LAppTextureManager.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/LAppTextureManager.hpp
      ${CMAKE_CURRENT_SOURCE_DIR}/LAppView.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/LAppView.hpp
      ${CMAKE_CURRENT_SOURCE_DIR}/processor/model/ModelRuntime.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/processor/model/ModelProcessor.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
  )

target_include_directories(${APP_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(${APP_NAME} deskwaifu_loader deskwaifu_renderer deskwaifu_ipc deskwaifu_input deskwaifu_output deskwaifu_compositor deskwaifu_kwin_cursor)
find_package(Qt6 6.5 REQUIRED COMPONENTS Gui OpenGL WaylandClient StateMachine)
add_library(deskwaifu_ocr_client STATIC processor/ocr/OcrClient.cpp processor/ocr/OcrClient.hpp)
set_target_properties(deskwaifu_ocr_client PROPERTIES AUTOMOC ON)
target_include_directories(deskwaifu_ocr_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/processor/ocr)
target_compile_definitions(deskwaifu_ocr_client PRIVATE
  DESKWAIFU_OCR_HELPER_PATH="${CMAKE_INSTALL_FULL_LIBEXECDIR}/deskwaifu-ocr")
target_link_libraries(deskwaifu_ocr_client PUBLIC Qt6::Core)
target_link_libraries(${APP_NAME} deskwaifu_ocr_client)
add_subdirectory(processor)
target_link_libraries(${APP_NAME} deskwaifu_speech)
target_link_libraries(${APP_NAME} deskwaifu_processors)
add_library(deskwaifu_window_state STATIC processor/interaction/ModelWindowState.cpp processor/interaction/ModelWindowState.hpp)
set_target_properties(deskwaifu_window_state PROPERTIES AUTOMOC ON)
target_include_directories(deskwaifu_window_state PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(deskwaifu_window_state PUBLIC Qt6::StateMachine)
find_package(LayerShellQt 6.6 REQUIRED)
add_library(deskwaifu_offscreen STATIC OffscreenRenderer.cpp render/ModelRenderTarget.cpp ReadRgbaFrame.cpp)
target_include_directories(deskwaifu_offscreen PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${GLEW_PATH}/include)
target_link_libraries(deskwaifu_offscreen PUBLIC glew_s OpenGL::OpenGL OpenGL::EGL Qt6::Gui Qt6::OpenGL)
target_sources(${APP_NAME} PRIVATE QtDesktopWindow.cpp)
target_link_libraries(${APP_NAME} deskwaifu_offscreen)
target_link_libraries(${APP_NAME} Qt6::Gui Qt6::OpenGL Qt6::WaylandClient LayerShellQt::Interface deskwaifu_window_state)
