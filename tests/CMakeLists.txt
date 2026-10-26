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

find_package(Threads REQUIRED)
find_package(Qt6 6.5 REQUIRED COMPONENTS Gui OpenGL WaylandClient StateMachine)
if(DESKWAIFU_BUILD_SPEECH_PLUGIN)
  find_package(Qt6 6.5 REQUIRED COMPONENTS WebSockets Multimedia)
endif()
find_package(LayerShellQt 6.6 REQUIRED)
add_executable(offscreen-renderer-test OffscreenRendererTest.cpp)
target_link_libraries(offscreen-renderer-test PRIVATE deskwaifu_offscreen)
add_test(NAME render-device-validation COMMAND offscreen-renderer-test --validation)
add_test(NAME render-device-startup
  COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:${APP_NAME}>
    -DFIXTURE_DIR=${CMAKE_CURRENT_BINARY_DIR}/render-device-startup
    -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckRenderDeviceStartup.cmake)
add_test(NAME offscreen-renderer COMMAND offscreen-renderer-test)
set_tests_properties(offscreen-renderer PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
add_executable(gpu-transfer-test GpuTransferTest.cpp ../src/processor/interaction/window/QtDesktopWindow.cpp)
target_link_libraries(gpu-transfer-test PRIVATE deskwaifu_offscreen Qt6::Gui Qt6::OpenGL Qt6::WaylandClient LayerShellQt::Interface ${CMAKE_DL_LIBS})
target_link_options(gpu-transfer-test PRIVATE -Wl,--export-dynamic-symbol=eglSwapBuffers)
foreach(target deskwaifu_offscreen offscreen-renderer-test gpu-transfer-test)
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${target}-includes.txt"
    CONTENT "$<JOIN:$<TARGET_PROPERTY:${target},INCLUDE_DIRECTORIES>,\n>\n")
endforeach()
add_test(NAME offscreen-bundled-glew
  COMMAND ${CMAKE_COMMAND} -DINCLUDE_DIR=${CMAKE_CURRENT_BINARY_DIR}
    -DGLEW_INCLUDE=${GLEW_PATH}/include
    -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckOffscreenGlewInclude.cmake)
add_executable(model-window-state-test ModelWindowStateTest.cpp)
target_link_libraries(model-window-state-test PRIVATE deskwaifu_window_state)
add_test(NAME model-window-state COMMAND model-window-state-test)
add_test(NAME qt-layer-shell-version
  COMMAND ${CMAKE_COMMAND} -DAPP_SOURCE=${CMAKE_SOURCE_DIR}/src
    -DFIXTURE_DIR=${CMAKE_CURRENT_BINARY_DIR}/layer-shell-version
    -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckLayerShellQtVersion.cmake)
add_executable(qt-window-input-test QtWindowInputTest.cpp)
target_include_directories(qt-window-input-test PRIVATE ../src)
target_link_libraries(qt-window-input-test PRIVATE Qt6::Gui)
add_test(NAME qt-window-input COMMAND qt-window-input-test)

add_executable(qt-desktop-window-test QtDesktopWindowTest.cpp ../src/processor/interaction/window/QtDesktopWindow.cpp)
target_include_directories(qt-desktop-window-test PRIVATE ../src)
target_link_libraries(qt-desktop-window-test PRIVATE Qt6::Gui Qt6::OpenGL Qt6::WaylandClient LayerShellQt::Interface OpenGL::EGL ${CMAKE_DL_LIBS})
target_link_libraries(qt-desktop-window-test PRIVATE deskwaifu_offscreen)
target_link_options(qt-desktop-window-test PRIVATE -Wl,--export-dynamic-symbol=eglSwapBuffers)
add_test(NAME qt-window-reject-platform COMMAND qt-desktop-window-test --reject-platform)
set_tests_properties(qt-window-reject-platform PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
find_program(WAYLAND_SCANNER wayland-scanner)
if (NOT WAYLAND_SCANNER)
  message(FATAL_ERROR "The desktop regression fixture requires wayland-scanner")
endif ()
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/fake-input-client.h ${CMAKE_CURRENT_BINARY_DIR}/fake-input-protocol.c
  COMMAND ${WAYLAND_SCANNER} client-header ${CMAKE_CURRENT_SOURCE_DIR}/fake-input.xml ${CMAKE_CURRENT_BINARY_DIR}/fake-input-client.h
  COMMAND ${WAYLAND_SCANNER} private-code ${CMAKE_CURRENT_SOURCE_DIR}/fake-input.xml ${CMAKE_CURRENT_BINARY_DIR}/fake-input-protocol.c
  DEPENDS fake-input.xml)
add_executable(click-through-test ClickThroughTest.cpp ../src/processor/interaction/window/QtDesktopWindow.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/fake-input-client.h ${CMAKE_CURRENT_BINARY_DIR}/fake-input-protocol.c)
target_include_directories(click-through-test PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(click-through-test PRIVATE deskwaifu_offscreen Qt6::WaylandClient LayerShellQt::Interface PkgConfig::WAYLAND_CLIENT)
# 仅显式启用时运行：使用隔离合成器，不向用户会话注入输入。
option(DESKWAIFU_QT_LIVE_TEST "Run Qt window integration test on the selected Wayland display" OFF)
if (DESKWAIFU_QT_LIVE_TEST)
  add_test(NAME gpu-transfer COMMAND gpu-transfer-test)
  set_tests_properties(gpu-transfer PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
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

add_executable(json-command-test JsonCommandTest.cpp ../src/contracts/ConfigStore.cpp)
target_include_directories(json-command-test PRIVATE ../src)
add_test(NAME json-command COMMAND json-command-test)

add_executable(config-store-test ConfigStoreTest.cpp ../src/contracts/ConfigStore.cpp)
target_include_directories(config-store-test PRIVATE ../src)
target_link_options(config-store-test PRIVATE -Wl,--wrap=rename)
add_test(NAME config-store COMMAND config-store-test)

add_executable(transform-editor-test TransformEditorTest.cpp ../src/contracts/ConfigStore.cpp ../src/processor/interaction/config/LAppConfig.cpp ../src/processor/interaction/InputController.cpp)
target_include_directories(transform-editor-test PRIVATE ../src)
add_test(NAME transform-editor COMMAND transform-editor-test)
add_test(NAME config-temp-directory
  COMMAND ${CMAKE_COMMAND}
    -DTEST_TEMP_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tmp
    -DCONFIG_TEST=$<TARGET_FILE:config-store-test>
    -DEDITOR_TEST=$<TARGET_FILE:transform-editor-test>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckPortableTemporaryDirectory.cmake)

add_executable(lapp-config-test LAppConfigTest.cpp ../src/processor/interaction/config/LAppConfig.cpp ../src/contracts/ConfigStore.cpp)
target_include_directories(lapp-config-test PRIVATE ../src)
add_test(NAME lapp-config COMMAND lapp-config-test)

add_executable(model-region-test ModelRegionTest.cpp ../src/processor/interaction/config/LAppConfig.cpp ../src/contracts/ConfigStore.cpp)
target_include_directories(model-region-test PRIVATE ../src)
add_test(NAME model-region COMMAND model-region-test)

add_executable(model-input-region-test ModelInputRegionTest.cpp)
target_include_directories(model-input-region-test PRIVATE ../src)
add_test(NAME model-input-region COMMAND model-input-region-test)

add_executable(hover-opacity-test HoverOpacityTest.cpp ../src/processor/interaction/config/LAppConfig.cpp ../src/contracts/ConfigStore.cpp)
target_include_directories(hover-opacity-test PRIVATE ../src)
add_test(NAME hover-opacity COMMAND hover-opacity-test)

add_executable(hover-opacity-settings-test HoverOpacitySettingsTest.cpp ../src/contracts/ConfigStore.cpp ../src/processor/interaction/config/LAppConfig.cpp)
target_include_directories(hover-opacity-settings-test PRIVATE ../src)
add_test(NAME hover-opacity-settings COMMAND hover-opacity-settings-test)

add_executable(mouse-look-settings-test MouseLookSettingsTest.cpp ../src/contracts/ConfigStore.cpp ../src/processor/interaction/config/LAppConfig.cpp)
target_include_directories(mouse-look-settings-test PRIVATE ../src)
add_test(NAME mouse-look-settings COMMAND mouse-look-settings-test)
add_executable(render-device-settings-test RenderDeviceSettingsTest.cpp ../src/contracts/ConfigStore.cpp ../src/processor/interaction/config/LAppConfig.cpp)
target_include_directories(render-device-settings-test PRIVATE ../src)
add_test(NAME render-device-settings COMMAND render-device-settings-test)

add_executable(model-region-render-test ModelRegionRenderTest.cpp ../src/processor/model/ModelFrameRegion.cpp)
target_include_directories(model-region-render-test PRIVATE ../src ${GLEW_PATH}/include)
target_link_libraries(model-region-render-test PRIVATE Framework glew_s OpenGL::EGL)
add_test(NAME model-region-render COMMAND model-region-render-test)
set_tests_properties(model-region-render PROPERTIES
  ENVIRONMENT "EGL_PLATFORM=surfaceless" SKIP_RETURN_CODE 77)

add_executable(lapp-sprite-test LAppSpriteTest.cpp ../src/processor/model/LAppSprite.cpp
  ${COMMON_SRC_PATH}/LAppSprite_Common.cpp)
target_include_directories(lapp-sprite-test PRIVATE ../src ../src/processor/model ${COMMON_SRC_PATH})
target_link_libraries(lapp-sprite-test PRIVATE Framework glew_s OpenGL::EGL)
add_test(NAME lapp-sprite COMMAND lapp-sprite-test)
set_tests_properties(lapp-sprite PROPERTIES
  ENVIRONMENT "EGL_PLATFORM=surfaceless" SKIP_RETURN_CODE 77)

add_executable(edit-input-region-test EditInputRegionTest.cpp
  ../src/contracts/ConfigStore.cpp ../src/processor/interaction/config/LAppConfig.cpp ../src/processor/interaction/InputController.cpp)
target_include_directories(edit-input-region-test PRIVATE ../src)
target_link_libraries(edit-input-region-test PRIVATE Qt6::Gui)
add_test(NAME edit-input-region COMMAND edit-input-region-test)

add_executable(failure-diagnostics-test FailureDiagnosticsTest.cpp)
target_include_directories(failure-diagnostics-test PRIVATE ../src)
add_test(NAME failure-diagnostics COMMAND failure-diagnostics-test)

find_package(Python3 COMPONENTS Interpreter)
if (DESKWAIFU_ENABLE_SANITIZERS)
  if (NOT Python3_Interpreter_FOUND)
    message(FATAL_ERROR "The sanitizer regression fixture requires Python")
  endif ()
  add_executable(sanitizer-failure-fixture SanitizerFailureFixture.cpp)
  add_test(NAME sanitizer-failure-gate COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_sanitizer_failure.py $<TARGET_FILE:sanitizer-failure-fixture>)
  set_tests_properties(sanitizer-failure-gate PROPERTIES TIMEOUT 30)
endif ()
if (Python3_Interpreter_FOUND)
  foreach(script smoke_isolation test_environment test_gate aur_packaging)
    string(REPLACE "_" "-" test_name ${script})
    add_test(NAME ${test_name} COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_${script}.py)
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 30)
  endforeach()
  add_test(NAME processor-boundaries COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_processor_boundaries.py)
  add_test(NAME ctl COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_ctl.py)
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

add_executable(ocr-client-test ${CMAKE_SOURCE_DIR}/features/ocr/tests/OcrClientTest.cpp)
target_link_libraries(ocr-client-test PRIVATE deskwaifu_ocr_client)
add_test(NAME ocr-client COMMAND ocr-client-test $<TARGET_FILE:processor-ocr-fixture>)
set_tests_properties(ocr-client PROPERTIES TIMEOUT 30)

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
add_test(NAME model-hotkeys-header
  COMMAND ${CMAKE_COMMAND} -DAPP_SOURCE=${CMAKE_SOURCE_DIR}/src
    -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DFIXTURE_SOURCE=${CMAKE_CURRENT_BINARY_DIR}/hotkeys-header
    -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckModelHotkeysHeader.cmake)

add_executable(ipc-transport-test IpcTransportTest.cpp)
target_include_directories(ipc-transport-test PRIVATE ../src)
target_link_libraries(ipc-transport-test PRIVATE deskwaifu_ipc)
add_test(NAME ipc-transport COMMAND ipc-transport-test)
add_executable(ipc-bounded-transport-test IpcBoundedTransportTest.cpp)
target_include_directories(ipc-bounded-transport-test PRIVATE ../src)
target_link_libraries(ipc-bounded-transport-test PRIVATE deskwaifu_ipc)
target_link_options(ipc-bounded-transport-test PRIVATE -Wl,--wrap=send -Wl,--wrap=read -Wl,--wrap=accept4)
add_test(NAME ipc-bounded-transport COMMAND ipc-bounded-transport-test)
set_tests_properties(ipc-bounded-transport PROPERTIES TIMEOUT 30)
add_test(NAME ipc-transport-header
  COMMAND ${CMAKE_COMMAND} -DAPP_SOURCE=${CMAKE_SOURCE_DIR}/src
    -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DFIXTURE_SOURCE=${CMAKE_CURRENT_BINARY_DIR}/transport-header
    -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckIpcTransportHeader.cmake)

add_executable(input-controller-test InputControllerTest.cpp)
target_link_libraries(input-controller-test PRIVATE deskwaifu_input)
add_test(NAME input-controller COMMAND input-controller-test)

add_executable(cursor-look-test CursorLookTest.cpp)
target_link_libraries(cursor-look-test PRIVATE deskwaifu_input)
add_test(NAME cursor-look COMMAND cursor-look-test)

add_executable(model-gaze-test ModelGazeTest.cpp
  ../src/processor/model/LAppModel.cpp ../src/processor/model/ModelHotkeys.cpp ../src/processor/model/LAppPal.cpp
  ../src/processor/model/LAppDefine.cpp ../src/processor/model/LAppTextureManager.cpp
  ${COMMON_SRC_PATH}/LAppAllocator_Common.cpp
  ${COMMON_SRC_PATH}/LAppModel_Common.cpp
  ${COMMON_SRC_PATH}/LAppTextureManager_Common.cpp
  ${COMMON_SRC_PATH}/LAppWavFileHandler_Common.cpp)
target_include_directories(model-gaze-test PRIVATE ../src ../src/processor/model ${COMMON_SRC_PATH}
  ${STB_PATH} ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(model-gaze-test PRIVATE Framework OpenGL::EGL deskwaifu_input)
target_compile_definitions(model-gaze-test PRIVATE SDK_RESOURCES="${SDK_ROOT_PATH}/Samples/Resources")
add_test(NAME model-gaze COMMAND model-gaze-test)
set_tests_properties(model-gaze PROPERTIES
  ENVIRONMENT "EGL_PLATFORM=surfaceless" SKIP_RETURN_CODE 77)

# 使用与正式程序相同的核心目标，只替换窗口边界。
add_executable(delegate-keyboard-test DelegateKeyboardTest.cpp)
target_link_libraries(delegate-keyboard-test PRIVATE deskwaifu_application OpenGL::EGL)
target_compile_definitions(delegate-keyboard-test PRIVATE SDK_RESOURCES="${SDK_ROOT_PATH}/Samples/Resources")
target_link_options(delegate-keyboard-test PRIVATE
  -Wl,--wrap=_ZN12ModelHotkeys7TriggerERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERS5_)
add_test(NAME delegate-keyboard COMMAND delegate-keyboard-test)
set_tests_properties(delegate-keyboard PROPERTIES ENVIRONMENT "EGL_PLATFORM=surfaceless" TIMEOUT 30)
if(DESKWAIFU_BUILD_SPEECH_PLUGIN)
  add_test(NAME delegate-plugin-commands COMMAND delegate-keyboard-test $<TARGET_FILE:speech_plugin>)
  set_tests_properties(delegate-plugin-commands PROPERTIES ENVIRONMENT "EGL_PLATFORM=surfaceless" TIMEOUT 30 LABELS "contract;plugin")
endif()

add_executable(application-service-test ApplicationServiceTest.cpp)
target_include_directories(application-service-test PRIVATE ../src)
add_test(NAME application-service COMMAND application-service-test)

add_executable(external-control-test ExternalControlTest.cpp)
target_include_directories(external-control-test PRIVATE ../src)
add_test(NAME external-control COMMAND external-control-test)

# 测试真实应用接线，只替换窗口、上下文和合成器套接字，
# 不替换 Delegate、View、Manager 或模型命中测试。
add_executable(model-pointer-test ModelPointerTest.cpp)
add_executable(model-render-thread-test ModelRenderThreadTest.cpp)
target_link_libraries(model-render-thread-test PRIVATE deskwaifu_application)
target_link_options(model-render-thread-test PRIVATE
  -Wl,--wrap=_ZNK17LAppLive2DManager6OnDragEff -Wl,--wrap=_ZN9LAppModel6UpdateERK15ModelFrameInput
  -Wl,--wrap=glGenTextures -Wl,--wrap=glDeleteTextures)
target_compile_definitions(model-render-thread-test PRIVATE SDK_RESOURCES="${SDK_ROOT_PATH}/Samples/Resources")
add_test(NAME model-render-thread COMMAND model-render-thread-test)
add_test(NAME model-shutdown-context COMMAND model-render-thread-test shutdown-context)
set_tests_properties(model-shutdown-context PROPERTIES ENVIRONMENT "EGL_PLATFORM=surfaceless" TIMEOUT 30)
set_tests_properties(model-render-thread PROPERTIES ENVIRONMENT "EGL_PLATFORM=surfaceless" TIMEOUT 30)
target_include_directories(model-pointer-test PRIVATE ../src ../src/processor/model ${COMMON_SRC_PATH}
  ${STB_PATH} ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(model-pointer-test PRIVATE deskwaifu_application OpenGL::EGL)
target_compile_definitions(model-pointer-test PRIVATE SDK_RESOURCES="${SDK_ROOT_PATH}/Samples/Resources")
target_link_options(model-pointer-test PRIVATE
  -Wl,--wrap=_ZN7LAppPal10PrintLogLnEPKcz -Wl,--wrap=_ZN9LAppModel19SetRandomExpressionEv
  -Wl,--wrap=_ZN17KWinCursorTracker5StartEv
  -Wl,--wrap=_ZN17KWinCursorTracker4PollERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE
  -Wl,--wrap=_ZNK17KWinCursorTracker13LocalPositionEiiRdS0_)
foreach(scenario release transform drag state cursor)
  add_test(NAME model-pointer-${scenario} COMMAND model-pointer-test ${scenario})
  set_tests_properties(model-pointer-${scenario} PROPERTIES
    ENVIRONMENT "EGL_PLATFORM=surfaceless" SKIP_RETURN_CODE 77 TIMEOUT 30)
endforeach()

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

add_executable(output-settings-test OutputSettingsTest.cpp ../src/contracts/ConfigStore.cpp ../src/processor/interaction/config/LAppConfig.cpp ../src/processor/interaction/window/OutputController.cpp)
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

set(SYSTEM_GUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src/plugins/system/gui")
set(INTERACTION_GUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src/plugins/interaction/gui")
qt_add_executable(deskwaifu-launcher-test
  DeskWaifuLauncherTest.cpp ${SYSTEM_GUI_DIR}/DeskWaifuLauncher.cpp)
target_include_directories(deskwaifu-launcher-test PRIVATE ${SYSTEM_GUI_DIR})
target_link_libraries(deskwaifu-launcher-test PRIVATE Qt6::Test)
target_compile_definitions(deskwaifu-launcher-test PRIVATE
  SPAWN_BACKEND_FIXTURE="$<TARGET_FILE:spawn-backend-fixture>")
add_dependencies(deskwaifu-launcher-test spawn-backend-fixture)
set_target_properties(deskwaifu-launcher-test PROPERTIES AUTOMOC ON)
add_test(NAME deskwaifu-launcher COMMAND deskwaifu-launcher-test)

qt_add_executable(deskwaifu-respawner-test
  DeskWaifuRespawnerTest.cpp
  ${SYSTEM_GUI_DIR}/DeskWaifuRespawner.cpp ${SYSTEM_GUI_DIR}/DeskWaifuRespawner.hpp
  ../control/DeskWaifuClient.cpp ../control/DeskWaifuClient.hpp
  ${SYSTEM_GUI_DIR}/DeskWaifuLauncher.cpp)
target_include_directories(deskwaifu-respawner-test PRIVATE ../control ../src ${SYSTEM_GUI_DIR})
target_link_libraries(deskwaifu-respawner-test PRIVATE Qt6::Test Qt6::Network deskwaifu_plugin_sdk)
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
  deskwaifu_plugin_sdk
)

set_target_properties(deskwaifu-client-test PROPERTIES AUTOMOC ON)
add_test(NAME deskwaifu-client COMMAND deskwaifu-client-test)

qt_add_executable(model-hotkeys-test
  ModelHotkeysTest.cpp
  ../src/processor/model/ModelHotkeys.cpp
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
  ${INTERACTION_GUI_DIR}/HotkeyPanel.cpp
  ${INTERACTION_GUI_DIR}/HotkeyPanel.hpp
)
target_include_directories(hotkey-panel-test PRIVATE ${INTERACTION_GUI_DIR})
target_link_libraries(hotkey-panel-test PRIVATE Qt6::Test Qt6::Widgets)
set_target_properties(hotkey-panel-test PROPERTIES AUTOMOC ON)
add_test(NAME hotkey-panel COMMAND hotkey-panel-test)
set_tests_properties(hotkey-panel PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

if(DESKWAIFU_BUILD_CONTROL)
add_subdirectory(gui)
qt_add_executable(control-theme-test
  ControlThemeTest.cpp
  ${INTERACTION_GUI_DIR}/HotkeyPanel.cpp
  ${INTERACTION_GUI_DIR}/HotkeyPanel.hpp
)
target_include_directories(control-theme-test PRIVATE ../control ${INTERACTION_GUI_DIR})
target_link_libraries(control-theme-test PRIVATE Qt6::Test deskwaifu_control_shell)
add_dependencies(control-theme-test deskwaifu_control_plugins)
target_compile_definitions(control-theme-test PRIVATE DESKWAIFU_TEST_SPEECH_PLUGIN=$<BOOL:${DESKWAIFU_BUILD_SPEECH_PLUGIN}>)
set_target_properties(control-theme-test PROPERTIES AUTOMOC ON AUTOUIC ON
  AUTOUIC_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/../control")
add_test(NAME control-theme COMMAND control-theme-test)
set_tests_properties(control-theme PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

qt_add_executable(main-window-transform-test
  MainWindowTransformTest.cpp
  ../src/contracts/ConfigStore.cpp ../src/processor/interaction/config/LAppConfig.cpp ../src/processor/interaction/InputController.cpp
)
target_include_directories(main-window-transform-test PRIVATE ../control ../src)
target_link_libraries(main-window-transform-test PRIVATE Qt6::Test deskwaifu_control_shell)
add_dependencies(main-window-transform-test deskwaifu_control_plugins)
target_compile_definitions(main-window-transform-test PRIVATE DESKWAIFU_TEST_SPEECH_PLUGIN=$<BOOL:${DESKWAIFU_BUILD_SPEECH_PLUGIN}>)
set_target_properties(main-window-transform-test PROPERTIES AUTOMOC ON AUTOUIC ON
  AUTOUIC_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/../control")
add_test(NAME main-window-transform COMMAND main-window-transform-test)
endif()
if(DESKWAIFU_BUILD_SPEECH_PLUGIN)
foreach(speech_test SpeechPlayer MiniMaxSpeechSource AudioPlayer)
  qt_add_executable(${speech_test}-test ${speech_test}Test.cpp ../src/contracts/ConfigStore.cpp ../src/processor/interaction/config/LAppConfig.cpp)
  set_target_properties(${speech_test}-test PROPERTIES AUTOMOC ON)
  target_link_libraries(${speech_test}-test PRIVATE deskwaifu_speech Qt6::Test)
  add_test(NAME ${speech_test} COMMAND ${speech_test}-test)
endforeach()
qt_add_executable(SpeechSettings-test SpeechSettingsTest.cpp ../src/contracts/ConfigStore.cpp)
set_target_properties(SpeechSettings-test PROPERTIES AUTOMOC ON)
target_include_directories(SpeechSettings-test PRIVATE ../src)
target_link_libraries(SpeechSettings-test PRIVATE Qt6::Core Qt6::Test)
target_link_options(SpeechSettings-test PRIVATE -Wl,--wrap=mkstemp)
add_test(NAME SpeechSettings COMMAND SpeechSettings-test)
endif()
if(Python3_Interpreter_FOUND)
  add_test(NAME plugin-boundaries COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_plugin_boundaries.py)
endif()
if(CMAKE_READELF AND CMAKE_NM)
  add_test(NAME plugin-host-link COMMAND ${CMAKE_COMMAND}
    -DHOST=$<TARGET_FILE:deskwaifu> -DREADELF=${CMAKE_READELF} -DNM=${CMAKE_NM}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckPluginHostLink.cmake)
endif()
add_test(NAME plugin-no-speech-configure COMMAND ${CMAKE_COMMAND}
  -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DBINARY_DIR=${CMAKE_BINARY_DIR}/no-speech-configure
  -DSDK_ROOT=${SDK_ROOT_PATH} -DGLEW_ROOT=${GLEW_PATH} -DGENERATOR=${CMAKE_GENERATOR}
  -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckNoSpeechConfiguration.cmake)
if(DESKWAIFU_BUILD_CONTROL)
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
set_tests_properties(control-theme main-window-transform main-window-temp-directory main-window-long-temp-directory
  PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;DESKWAIFU_CONTROL_PLUGIN_DIR=${CMAKE_BINARY_DIR}/plugins")
endif()

qt_add_executable(model-keyboard-test ModelKeyboardTest.cpp)
target_include_directories(model-keyboard-test PRIVATE ../src)
target_link_libraries(model-keyboard-test PRIVATE Qt6::Test)
set_target_properties(model-keyboard-test PROPERTIES AUTOMOC ON)
add_test(NAME model-keyboard COMMAND model-keyboard-test)
