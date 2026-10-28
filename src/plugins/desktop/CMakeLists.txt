foreach(command toggle --toggle focus --focus)
  configure_file(cli/ControlCommand.sh "${CMAKE_BINARY_DIR}/plugins/desktop/commands/${command}"
    COPYONLY FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
  install(PROGRAMS cli/ControlCommand.sh
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/deskwaifu/plugins/desktop/commands RENAME "${command}" COMPONENT DeskWaifu)
endforeach()

find_package(Qt6 6.5 REQUIRED COMPONENTS Gui OpenGL WaylandClient StateMachine Concurrent)
find_package(LayerShellQt 6.6 REQUIRED)
find_package(OpenGL REQUIRED COMPONENTS OpenGL EGL)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
set(SDK_ROOT_PATH "${PROJECT_SOURCE_DIR}/CubismSdkForNative-5-r.5" CACHE PATH "Cubism SDK directory")
set(GLEW_PATH "${PROJECT_SOURCE_DIR}/thirdParty/glew" CACHE PATH "GLEW source directory")
set(CORE_PATH "${SDK_ROOT_PATH}/Core")
set(FRAMEWORK_PATH "${SDK_ROOT_PATH}/Framework")
set(COMMON_SRC_PATH "${SDK_ROOT_PATH}/Samples/Common")
set(STB_PATH "${PROJECT_SOURCE_DIR}/thirdParty/stb")
set(GLEW_EGL ON CACHE BOOL "" FORCE)
set(BUILD_UTILS OFF CACHE BOOL "" FORCE)
add_library(Live2DCubismCore STATIC IMPORTED GLOBAL)
set_target_properties(Live2DCubismCore PROPERTIES
  IMPORTED_LOCATION "${CORE_PATH}/lib/linux/x86_64/libLive2DCubismCore.a"
  INTERFACE_INCLUDE_DIRECTORIES "${CORE_PATH}/include")
add_subdirectory("${GLEW_PATH}/build/cmake" "${CMAKE_BINARY_DIR}/glew")
set(FRAMEWORK_SOURCE OpenGL)
add_subdirectory("${FRAMEWORK_PATH}" "${CMAKE_BINARY_DIR}/Framework")
target_compile_definitions(Framework PUBLIC CSM_TARGET_LINUX_GL)
target_include_directories(Framework PUBLIC "${GLEW_PATH}/include")
target_link_libraries(Framework Live2DCubismCore glew_s)

set(private "${CMAKE_CURRENT_SOURCE_DIR}/private")
add_library(deskwaifu_catalog STATIC private/processor/model/ModelCatalog.cpp)
add_library(deskwaifu_input STATIC private/processor/interaction/InputController.cpp)
add_library(deskwaifu_output STATIC private/processor/interaction/window/OutputController.cpp)
add_library(deskwaifu_compositor STATIC private/processor/interaction/window/CompositorAdapter.cpp)
target_link_libraries(deskwaifu_compositor PUBLIC Threads::Threads)
add_library(deskwaifu_kwin_cursor STATIC private/processor/input/KWinCursorTracker.cpp)
target_link_libraries(deskwaifu_kwin_cursor PUBLIC PkgConfig::SYSTEMD Qt6::Core)
add_library(deskwaifu_loader STATIC private/processor/model/ModelLoader.cpp)
target_link_libraries(deskwaifu_loader PUBLIC deskwaifu_catalog Framework)
add_library(deskwaifu_renderer STATIC private/processor/model/ModelRenderer.cpp)
target_link_libraries(deskwaifu_renderer PUBLIC Framework)
add_library(deskwaifu_window_state STATIC private/processor/interaction/ModelWindowState.cpp)
target_link_libraries(deskwaifu_window_state PUBLIC Qt6::StateMachine)
set_target_properties(deskwaifu_window_state PROPERTIES AUTOMOC ON)
add_library(deskwaifu_offscreen STATIC private/render/OffscreenRenderer.cpp
  private/render/ModelRenderTarget.cpp private/render/ReadRgbaFrame.cpp)
target_link_libraries(deskwaifu_offscreen PUBLIC glew_s OpenGL::OpenGL OpenGL::EGL Qt6::Gui Qt6::OpenGL)
foreach(target catalog input output compositor kwin_cursor loader renderer window_state offscreen)
  target_include_directories(deskwaifu_${target} PUBLIC "${private}")
  target_link_libraries(deskwaifu_${target} PUBLIC deskwaifu_plugin_sdk)
endforeach()
target_include_directories(deskwaifu_loader PRIVATE "${COMMON_SRC_PATH}")
target_include_directories(deskwaifu_renderer PRIVATE "${COMMON_SRC_PATH}")
target_include_directories(deskwaifu_offscreen PUBLIC "${GLEW_PATH}/include")

add_library(deskwaifu_application STATIC
  private/app/LAppDelegate.cpp private/app/DesktopStartup.cpp private/processor/model/LAppDefine.cpp
  private/processor/model/ModelFrameRegion.cpp private/processor/model/LAppLive2DManager.cpp
  private/processor/model/LAppModel.cpp private/processor/model/ModelHotkeys.cpp
  private/processor/model/LAppPal.cpp private/processor/model/LAppSprite.cpp
  private/processor/model/LAppSpriteShader.cpp private/processor/model/LAppTextureManager.cpp
  private/processor/model/LAppView.cpp private/processor/model/ModelRuntime.cpp
  private/processor/model/ModelProcessor.cpp private/processor/interaction/InteractionProcessor.cpp
  private/processor/interaction/InteractionContext.cpp private/processor/interaction/AppCommandHandler.cpp
  private/processor/interaction/config/LAppConfig.cpp private/processor/input/InputListener.cpp
  private/processor/signal/SignalProcessor.cpp)
set(APP_NAME deskwaifu_application)
add_subdirectory("${COMMON_SRC_PATH}" "${CMAKE_BINARY_DIR}/src/Common")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
execute_process(COMMAND bash "${PROJECT_SOURCE_DIR}/cmake/embed_shaders.sh"
  "${FRAMEWORK_PATH}/src/Rendering/OpenGL/Shaders/Standard"
  "${SDK_ROOT_PATH}/Samples/OpenGL/Shaders/Standard"
  "${CMAKE_BINARY_DIR}/generated/EmbeddedShaders.hpp" COMMAND_ERROR_IS_FATAL ANY)
target_include_directories(deskwaifu_application PUBLIC "${private}" "${private}/processor/model"
  "${COMMON_SRC_PATH}" "${STB_PATH}" "${CMAKE_BINARY_DIR}/generated")
target_compile_definitions(deskwaifu_application PRIVATE
  DESKWAIFU_PLUGIN_DIR="${CMAKE_INSTALL_FULL_LIBDIR}/deskwaifu/plugins")
target_link_libraries(deskwaifu_application PUBLIC deskwaifu_plugin_sdk
  deskwaifu_loader deskwaifu_renderer deskwaifu_input deskwaifu_output
  deskwaifu_compositor deskwaifu_kwin_cursor deskwaifu_offscreen deskwaifu_window_state
  Qt6::Gui Qt6::OpenGL Qt6::WaylandClient LayerShellQt::Interface)
deskwaifu_add_plugin(desktop DesktopPlugin.cpp private/processor/interaction/window/QtDesktopWindow.cpp)
target_link_libraries(desktop_plugin PRIVATE deskwaifu_application)
add_executable(deskwaifu-cursor-guard CursorGuardMain.cpp)
target_link_libraries(deskwaifu-cursor-guard PRIVATE deskwaifu_kwin_cursor)
add_dependencies(desktop_plugin deskwaifu-cursor-guard)
install(TARGETS deskwaifu-cursor-guard RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DeskWaifu)

foreach(variable CORE_PATH FRAMEWORK_PATH COMMON_SRC_PATH STB_PATH)
  set(${variable} "${${variable}}" PARENT_SCOPE)
endforeach()
