add_executable(component-dependencies-test ComponentDependenciesTest.cpp)
target_include_directories(component-dependencies-test PRIVATE ../src)
add_test(NAME component-dependencies COMMAND component-dependencies-test)
add_test(NAME component-direction
  COMMAND ${CMAKE_COMMAND} -DAPP_SOURCE=${CMAKE_SOURCE_DIR}/src
    -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckComponentDirection.cmake)
add_test(NAME component-direction-missing
  COMMAND ${CMAKE_COMMAND} -DAPP_SOURCE=${CMAKE_CURRENT_BINARY_DIR}/missing-components
    -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckComponentDirection.cmake)
set_tests_properties(component-direction-missing PROPERTIES WILL_FAIL TRUE)
add_test(NAME command-service-direction
  COMMAND ${CMAKE_COMMAND} -DAPP_SOURCE=${CMAKE_SOURCE_DIR}/src
    -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckCommandServiceDirection.cmake)
add_test(NAME command-service-direction-regression
  COMMAND ${CMAKE_COMMAND} -DFIXTURE_SOURCE=${CMAKE_CURRENT_BINARY_DIR}/reverse-port-fixture
    -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckCommandServiceDirectionRegression.cmake)
add_test(NAME cursor-look-wiring
  COMMAND ${CMAKE_COMMAND} -DAPP_SOURCE=${CMAKE_SOURCE_DIR}/src
    -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckCursorLookWiring.cmake)

find_package(Threads REQUIRED)
find_package(Qt6 6.5 REQUIRED COMPONENTS Gui OpenGL WaylandClient)
find_package(LayerShellQt 6.6 REQUIRED)
add_test(NAME qt-layer-shell-version
  COMMAND ${CMAKE_COMMAND} -DAPP_SOURCE=${CMAKE_SOURCE_DIR}/src
    -DFIXTURE_DIR=${CMAKE_CURRENT_BINARY_DIR}/layer-shell-version
    -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckLayerShellQtVersion.cmake)
add_executable(qt-window-input-test QtWindowInputTest.cpp)
target_include_directories(qt-window-input-test PRIVATE ../src)
target_link_libraries(qt-window-input-test PRIVATE Qt6::Gui)
add_test(NAME qt-window-input COMMAND qt-window-input-test)

add_executable(qt-desktop-window-test QtDesktopWindowTest.cpp ../src/QtDesktopWindow.cpp)
target_include_directories(qt-desktop-window-test PRIVATE ../src)
target_link_libraries(qt-desktop-window-test PRIVATE Qt6::Gui Qt6::OpenGL Qt6::WaylandClient LayerShellQt::Interface OpenGL::EGL ${CMAKE_DL_LIBS})
target_link_options(qt-desktop-window-test PRIVATE -Wl,--export-dynamic-symbol=eglSwapBuffers)
add_test(NAME qt-window-reject-platform COMMAND qt-desktop-window-test --reject-platform)
set_tests_properties(qt-window-reject-platform PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
# Opt-in only: use an isolated compositor, never inject input into the user's session.
option(DESKWAIFU_QT_LIVE_TEST "Run Qt window integration test on the selected Wayland display" OFF)
if (DESKWAIFU_QT_LIVE_TEST)
  add_test(NAME qt-desktop-window COMMAND qt-desktop-window-test)
  set_tests_properties(qt-desktop-window PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
endif ()
add_executable(kwin-cursor-tracker-test KWinCursorTrackerTest.cpp)
target_link_libraries(kwin-cursor-tracker-test PRIVATE deskwaifu_kwin_cursor Threads::Threads)
find_program(DBUS_RUN_SESSION dbus-run-session)
if (DBUS_RUN_SESSION)
  add_test(NAME kwin-cursor-tracker
    COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:kwin-cursor-tracker-test>)
endif ()
find_program(NODE_EXECUTABLE NAMES node nodejs)
if (NODE_EXECUTABLE)
  add_test(NAME kwin-cursor-script
    COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_kwin_cursor_script.js
      $<TARGET_FILE:kwin-cursor-tracker-test>)
endif ()

add_executable(json-command-test JsonCommandTest.cpp)
target_include_directories(json-command-test PRIVATE ../src)
add_test(NAME json-command COMMAND json-command-test)

add_executable(config-store-test ConfigStoreTest.cpp ../src/ConfigStore.cpp)
target_include_directories(config-store-test PRIVATE ../src)
add_test(NAME config-store COMMAND config-store-test)

add_executable(transform-editor-test TransformEditorTest.cpp ../src/ConfigStore.cpp ../src/LAppConfig.cpp ../src/InputController.cpp)
target_include_directories(transform-editor-test PRIVATE ../src)
add_test(NAME transform-editor COMMAND transform-editor-test)
add_test(NAME config-temp-directory
  COMMAND ${CMAKE_COMMAND}
    -DTEST_TEMP_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tmp
    -DCONFIG_TEST=$<TARGET_FILE:config-store-test>
    -DEDITOR_TEST=$<TARGET_FILE:transform-editor-test>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckPortableTemporaryDirectory.cmake)

add_executable(lapp-config-test LAppConfigTest.cpp ../src/LAppConfig.cpp ../src/ConfigStore.cpp)
target_include_directories(lapp-config-test PRIVATE ../src)
add_test(NAME lapp-config COMMAND lapp-config-test)

add_executable(model-region-test ModelRegionTest.cpp ../src/LAppConfig.cpp ../src/ConfigStore.cpp)
target_include_directories(model-region-test PRIVATE ../src)
add_test(NAME model-region COMMAND model-region-test)

add_executable(model-input-region-test ModelInputRegionTest.cpp)
target_include_directories(model-input-region-test PRIVATE ../src)
add_test(NAME model-input-region COMMAND model-input-region-test)

add_executable(hover-opacity-test HoverOpacityTest.cpp ../src/LAppConfig.cpp ../src/ConfigStore.cpp)
target_include_directories(hover-opacity-test PRIVATE ../src)
add_test(NAME hover-opacity COMMAND hover-opacity-test)

add_executable(hover-opacity-settings-test HoverOpacitySettingsTest.cpp ../src/ConfigStore.cpp ../src/LAppConfig.cpp)
target_include_directories(hover-opacity-settings-test PRIVATE ../src)
add_test(NAME hover-opacity-settings COMMAND hover-opacity-settings-test)

add_executable(mouse-look-settings-test MouseLookSettingsTest.cpp ../src/ConfigStore.cpp ../src/LAppConfig.cpp)
target_include_directories(mouse-look-settings-test PRIVATE ../src)
add_test(NAME mouse-look-settings COMMAND mouse-look-settings-test)

add_executable(model-region-render-test ModelRegionRenderTest.cpp ../src/ModelFrameRegion.cpp)
target_include_directories(model-region-render-test PRIVATE ../src ${GLEW_PATH}/include)
target_link_libraries(model-region-render-test PRIVATE Framework glew_s OpenGL::EGL)
add_test(NAME model-region-render COMMAND model-region-render-test)
set_tests_properties(model-region-render PROPERTIES
  ENVIRONMENT "EGL_PLATFORM=surfaceless" SKIP_RETURN_CODE 77)

add_executable(edit-input-region-test EditInputRegionTest.cpp
  ../src/ConfigStore.cpp ../src/LAppConfig.cpp ../src/InputController.cpp)
target_include_directories(edit-input-region-test PRIVATE ../src)
target_link_libraries(edit-input-region-test PRIVATE Qt6::Gui)
add_test(NAME edit-input-region COMMAND edit-input-region-test)

add_executable(failure-diagnostics-test FailureDiagnosticsTest.cpp)
target_include_directories(failure-diagnostics-test PRIVATE ../src)
add_test(NAME failure-diagnostics COMMAND failure-diagnostics-test)

find_package(Python3 COMPONENTS Interpreter)
if (Python3_Interpreter_FOUND)
  if (DBUS_RUN_SESSION)
    add_test(NAME kwin-cursor-locale
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_kwin_cursor_locale.py
        $<TARGET_FILE:kwin-cursor-tracker-test> ${DBUS_RUN_SESSION} ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(kwin-cursor-locale PROPERTIES SKIP_RETURN_CODE 77)
  endif ()
  add_test(NAME qt-window-configuration
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_qt_window_configuration.py)
  add_test(NAME opencode-review-cli
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/.gitea/scripts/test_opencode_review_cli.py)
  add_test(NAME opencode-review-extraction
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/.gitea/scripts/test_extract_opencode_review.py)
endif ()

add_executable(model-catalog-test ModelCatalogTest.cpp)
target_include_directories(model-catalog-test PRIVATE ../src)
target_link_libraries(model-catalog-test PRIVATE deskwaifu_catalog)
add_test(NAME model-catalog COMMAND model-catalog-test)

add_executable(model-cache-test ModelCacheTest.cpp)
target_include_directories(model-cache-test PRIVATE ../src)
add_test(NAME model-cache COMMAND model-cache-test)

add_executable(model-session-test ModelSessionTest.cpp)
target_include_directories(model-session-test PRIVATE ../src)
add_test(NAME model-session COMMAND model-session-test)

add_executable(ipc-transport-test IpcTransportTest.cpp)
target_include_directories(ipc-transport-test PRIVATE ../src)
target_link_libraries(ipc-transport-test PRIVATE deskwaifu_ipc)
add_test(NAME ipc-transport COMMAND ipc-transport-test)

add_executable(input-controller-test InputControllerTest.cpp)
target_link_libraries(input-controller-test PRIVATE deskwaifu_input)
add_test(NAME input-controller COMMAND input-controller-test)

add_executable(cursor-look-test CursorLookTest.cpp)
target_link_libraries(cursor-look-test PRIVATE deskwaifu_input)
add_test(NAME cursor-look COMMAND cursor-look-test)

add_executable(model-frame-test ModelFrameTest.cpp)
target_link_libraries(model-frame-test PRIVATE deskwaifu_input)
add_test(NAME model-frame COMMAND model-frame-test)

add_executable(model-gaze-test ModelGazeTest.cpp
  ../src/LAppModel.cpp ../src/ModelHotkeys.cpp ../src/LAppPal.cpp
  ../src/LAppDefine.cpp ../src/LAppTextureManager.cpp
  ${COMMON_SRC_PATH}/LAppAllocator_Common.cpp
  ${COMMON_SRC_PATH}/LAppModel_Common.cpp
  ${COMMON_SRC_PATH}/LAppTextureManager_Common.cpp
  ${COMMON_SRC_PATH}/LAppWavFileHandler_Common.cpp)
target_include_directories(model-gaze-test PRIVATE ../src ${COMMON_SRC_PATH}
  ${STB_PATH} ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(model-gaze-test PRIVATE Framework OpenGL::EGL deskwaifu_input)
target_compile_definitions(model-gaze-test PRIVATE SDK_RESOURCES="${SDK_ROOT_PATH}/Samples/Resources")
add_test(NAME model-gaze COMMAND model-gaze-test)
set_tests_properties(model-gaze PROPERTIES
  ENVIRONMENT "EGL_PLATFORM=surfaceless" SKIP_RETURN_CODE 77)

add_executable(application-service-test ApplicationServiceTest.cpp)
target_include_directories(application-service-test PRIVATE ../src)
add_test(NAME application-service COMMAND application-service-test)

add_executable(external-control-test ExternalControlTest.cpp)
target_include_directories(external-control-test PRIVATE ../src)
add_test(NAME external-control COMMAND external-control-test)

add_executable(texture-residency-test TextureResidencyTest.cpp)
target_include_directories(texture-residency-test PRIVATE ../src ${GLEW_PATH}/include ${FRAMEWORK_PATH}/src ${CORE_PATH}/include ${COMMON_SRC_PATH})
add_test(NAME texture-residency COMMAND texture-residency-test)
add_test(NAME smoke-exit-status
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/SmokeExitStatusTest.sh ${CMAKE_SOURCE_DIR})
add_test(NAME ci-image-private-transfer
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/CiImagePrivateTransferTest.sh ${CMAKE_SOURCE_DIR})

add_executable(output-controller-test OutputControllerTest.cpp)
target_link_libraries(output-controller-test PRIVATE deskwaifu_output)
add_test(NAME output-controller COMMAND output-controller-test)

add_executable(output-settings-test OutputSettingsTest.cpp ../src/ConfigStore.cpp ../src/LAppConfig.cpp ../src/OutputController.cpp)
target_include_directories(output-settings-test PRIVATE ../src)
target_link_options(output-settings-test PRIVATE -Wl,--wrap=mkstemp)
add_test(NAME output-settings COMMAND output-settings-test)

add_executable(compositor-adapter-test CompositorAdapterTest.cpp)
target_link_libraries(compositor-adapter-test PRIVATE deskwaifu_compositor)
target_link_options(compositor-adapter-test PRIVATE -Wl,--wrap=waitpid)
add_test(NAME compositor-adapter COMMAND compositor-adapter-test)

if (NOT DESKWAIFU_BUILD_CONTROL)
  return()
endif ()

find_package(Qt6 REQUIRED COMPONENTS Test Network Widgets)

qt_add_executable(spawn-backend-fixture SpawnBackendFixture.cpp)
target_link_libraries(spawn-backend-fixture PRIVATE Qt6::Core)
set_target_properties(spawn-backend-fixture PROPERTIES
  OUTPUT_NAME deskwaifu
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/spawn-fixture")

qt_add_executable(deskwaifu-launcher-test
  DeskWaifuLauncherTest.cpp ../control/DeskWaifuLauncher.cpp)
target_include_directories(deskwaifu-launcher-test PRIVATE ../control)
target_link_libraries(deskwaifu-launcher-test PRIVATE Qt6::Test)
target_compile_definitions(deskwaifu-launcher-test PRIVATE
  SPAWN_BACKEND_FIXTURE="$<TARGET_FILE:spawn-backend-fixture>")
add_dependencies(deskwaifu-launcher-test spawn-backend-fixture)
set_target_properties(deskwaifu-launcher-test PROPERTIES AUTOMOC ON)
add_test(NAME deskwaifu-launcher COMMAND deskwaifu-launcher-test)

qt_add_executable(deskwaifu-respawner-test
  DeskWaifuRespawnerTest.cpp
  ../control/DeskWaifuRespawner.cpp ../control/DeskWaifuRespawner.hpp
  ../control/DeskWaifuClient.cpp ../control/DeskWaifuClient.hpp
  ../control/DeskWaifuLauncher.cpp)
target_include_directories(deskwaifu-respawner-test PRIVATE ../control ../src)
target_link_libraries(deskwaifu-respawner-test PRIVATE Qt6::Test Qt6::Network)
target_compile_definitions(deskwaifu-respawner-test PRIVATE
  SPAWN_BACKEND_FIXTURE="$<TARGET_FILE:spawn-backend-fixture>")
add_dependencies(deskwaifu-respawner-test spawn-backend-fixture)
set_target_properties(deskwaifu-respawner-test PROPERTIES AUTOMOC ON)
add_test(NAME deskwaifu-respawner COMMAND deskwaifu-respawner-test)

qt_add_executable(deskwaifu-client-test
  DeskWaifuClientTest.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../control/DeskWaifuClient.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../control/DeskWaifuClient.hpp
)

target_include_directories(deskwaifu-client-test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../control
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_link_libraries(deskwaifu-client-test PRIVATE
  Qt6::Test
  Qt6::Network
)

set_target_properties(deskwaifu-client-test PROPERTIES AUTOMOC ON)
add_test(NAME deskwaifu-client COMMAND deskwaifu-client-test)

qt_add_executable(model-hotkeys-test
  ModelHotkeysTest.cpp
  ../src/ModelHotkeys.cpp
  ${COMMON_SRC_PATH}/LAppAllocator_Common.cpp
)
target_include_directories(model-hotkeys-test PRIVATE ../src ${COMMON_SRC_PATH})
target_compile_definitions(model-hotkeys-test PRIVATE SDK_RESOURCES="${SDK_ROOT_PATH}/Samples/Resources")
target_link_libraries(model-hotkeys-test PRIVATE Qt6::Test Framework)
set_target_properties(model-hotkeys-test PROPERTIES AUTOMOC ON)
add_test(NAME model-hotkeys COMMAND model-hotkeys-test)

qt_add_executable(model-transform-test ModelTransformTest.cpp)
target_include_directories(model-transform-test PRIVATE ../src)
target_link_libraries(model-transform-test PRIVATE Qt6::Test Framework)
set_target_properties(model-transform-test PROPERTIES AUTOMOC ON)
add_test(NAME model-transform COMMAND model-transform-test)

qt_add_executable(hotkey-panel-test
  HotkeyPanelTest.cpp
  ../control/HotkeyPanel.cpp
  ../control/HotkeyPanel.hpp
)
target_include_directories(hotkey-panel-test PRIVATE ../control)
target_link_libraries(hotkey-panel-test PRIVATE Qt6::Test Qt6::Widgets)
set_target_properties(hotkey-panel-test PROPERTIES AUTOMOC ON)
add_test(NAME hotkey-panel COMMAND hotkey-panel-test)
set_tests_properties(hotkey-panel PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

qt_add_executable(control-theme-test
  ControlThemeTest.cpp
  ../control/MainWindow.ui
  ../control/HotkeyPanel.cpp
  ../control/HotkeyPanel.hpp
)
target_include_directories(control-theme-test PRIVATE ../control)
target_link_libraries(control-theme-test PRIVATE Qt6::Test Qt6::Widgets)
set_target_properties(control-theme-test PROPERTIES AUTOMOC ON AUTOUIC ON
  AUTOUIC_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/../control")
add_test(NAME control-theme COMMAND control-theme-test)
set_tests_properties(control-theme PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

qt_add_executable(main-window-transform-test
  MainWindowTransformTest.cpp
  ../control/MainWindow.cpp ../control/MainWindow.hpp ../control/MainWindow.ui
  ../control/DeskWaifuClient.cpp ../control/DeskWaifuClient.hpp
  ../control/DeskWaifuLauncher.cpp ../control/DeskWaifuLauncher.hpp
  ../control/DeskWaifuRespawner.cpp ../control/DeskWaifuRespawner.hpp
  ../control/HotkeyPanel.cpp ../control/HotkeyPanel.hpp
  ../src/ConfigStore.cpp ../src/LAppConfig.cpp ../src/InputController.cpp
)
target_include_directories(main-window-transform-test PRIVATE ../control ../src)
target_link_libraries(main-window-transform-test PRIVATE Qt6::Test Qt6::Widgets Qt6::Network)
set_target_properties(main-window-transform-test PROPERTIES AUTOMOC ON AUTOUIC ON
  AUTOUIC_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/../control")
add_test(NAME main-window-transform COMMAND main-window-transform-test)
set_tests_properties(main-window-transform PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_test(NAME main-window-temp-directory
  COMMAND ${CMAKE_COMMAND}
    -DTEST_TEMP_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tmp
    -DWINDOW_TEST=$<TARGET_FILE:main-window-transform-test>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckPortableTemporaryDirectory.cmake)
string(REPEAT "long-build-path-" 10 LONG_TEMP_SUFFIX)
add_test(NAME main-window-long-temp-directory
  COMMAND ${CMAKE_COMMAND}
    -DTEST_TEMP_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tmp/${LONG_TEMP_SUFFIX}
    -DEXPECT_LONG_TEMP=ON
    -DWINDOW_TEST=$<TARGET_FILE:main-window-transform-test>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckPortableTemporaryDirectory.cmake)

qt_add_executable(model-keyboard-test ModelKeyboardTest.cpp)
target_include_directories(model-keyboard-test PRIVATE ../src ${FRAMEWORK_PATH}/src ${CORE_PATH}/include)
target_link_libraries(model-keyboard-test PRIVATE Qt6::Test)
set_target_properties(model-keyboard-test PROPERTIES AUTOMOC ON)
add_test(NAME model-keyboard COMMAND model-keyboard-test)
