find_package(Threads REQUIRED)
find_package(Qt6 6.5 REQUIRED COMPONENTS Gui OpenGL WaylandClient StateMachine)
find_package(LayerShellQt 6.6 REQUIRED)
set(PRIVATE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../private")
set(TEST_SCRIPTS "${CMAKE_SOURCE_DIR}/tests")

add_executable(model-input-region-test ModelInputRegionTest.cpp)
add_test(NAME model-input-region COMMAND model-input-region-test)
add_executable(model-hit-test-test ModelHitTestTest.cpp)
target_link_libraries(model-hit-test-test PRIVATE Threads::Threads)
add_test(NAME model-hit-test COMMAND model-hit-test-test)

add_executable(component-dependencies-test ComponentDependenciesTest.cpp)
add_test(NAME component-dependencies COMMAND component-dependencies-test)
add_test(NAME component-direction COMMAND ${CMAKE_COMMAND}
  -DAPP_SOURCE=${PRIVATE_ROOT} -P ${TEST_SCRIPTS}/CheckComponentDirection.cmake)
add_test(NAME component-direction-missing COMMAND ${CMAKE_COMMAND}
  -DAPP_SOURCE=${CMAKE_CURRENT_BINARY_DIR}/missing-components
  -P ${TEST_SCRIPTS}/CheckComponentDirection.cmake)
set_tests_properties(component-direction-missing PROPERTIES WILL_FAIL TRUE)
add_test(NAME command-service-direction COMMAND ${CMAKE_COMMAND}
  -DAPP_SOURCE=${PRIVATE_ROOT} -P ${TEST_SCRIPTS}/CheckCommandServiceDirection.cmake)
add_test(NAME command-service-direction-regression COMMAND ${CMAKE_COMMAND}
  -DFIXTURE_SOURCE=${CMAKE_CURRENT_BINARY_DIR}/reverse-port-fixture
  -P ${TEST_SCRIPTS}/CheckCommandServiceDirectionRegression.cmake)

add_executable(offscreen-renderer-test OffscreenRendererTest.cpp)
target_link_libraries(offscreen-renderer-test PRIVATE deskwaifu_offscreen)
add_test(NAME render-device-validation COMMAND offscreen-renderer-test --validation)
add_test(NAME render-device-startup COMMAND ${CMAKE_COMMAND}
  -DAPP=$<TARGET_FILE:deskwaifu> -DFIXTURE_DIR=${CMAKE_CURRENT_BINARY_DIR}/render-device-startup
  -P ${TEST_SCRIPTS}/CheckRenderDeviceStartup.cmake)
set_tests_properties(render-device-startup PROPERTIES TIMEOUT 30
  ENVIRONMENT "DESKWAIFU_PLUGIN_DIR=${CMAKE_BINARY_DIR}/plugins")
add_test(NAME offscreen-renderer COMMAND offscreen-renderer-test)
set_tests_properties(offscreen-renderer PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
add_executable(gpu-transfer-test GpuTransferTest.cpp ${PRIVATE_ROOT}/processor/interaction/window/QtDesktopWindow.cpp)
target_link_libraries(gpu-transfer-test PRIVATE deskwaifu_offscreen Qt6::Gui Qt6::OpenGL Qt6::WaylandClient LayerShellQt::Interface ${CMAKE_DL_LIBS})
target_link_options(gpu-transfer-test PRIVATE -Wl,--export-dynamic-symbol=eglSwapBuffers)
foreach(target deskwaifu_offscreen offscreen-renderer-test gpu-transfer-test)
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${target}-includes.txt"
    CONTENT "$<JOIN:$<TARGET_PROPERTY:${target},INCLUDE_DIRECTORIES>,\n>\n")
endforeach()
add_test(NAME offscreen-bundled-glew COMMAND ${CMAKE_COMMAND}
  -DINCLUDE_DIR=${CMAKE_CURRENT_BINARY_DIR} -DGLEW_INCLUDE=${GLEW_PATH}/include
  -P ${TEST_SCRIPTS}/CheckOffscreenGlewInclude.cmake)
add_executable(model-window-state-test ModelWindowStateTest.cpp)
target_link_libraries(model-window-state-test PRIVATE deskwaifu_window_state)
add_test(NAME model-window-state COMMAND model-window-state-test)
add_test(NAME qt-layer-shell-version COMMAND ${CMAKE_COMMAND}
  -DAPP_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/..
  -DFIXTURE_DIR=${CMAKE_CURRENT_BINARY_DIR}/layer-shell-version
  -P ${TEST_SCRIPTS}/CheckLayerShellQtVersion.cmake)
add_executable(qt-window-input-test QtWindowInputTest.cpp)
target_link_libraries(qt-window-input-test PRIVATE Qt6::Gui)
add_test(NAME qt-window-input COMMAND qt-window-input-test)
add_executable(qt-desktop-window-test QtDesktopWindowTest.cpp ${PRIVATE_ROOT}/processor/interaction/window/QtDesktopWindow.cpp)
target_link_libraries(qt-desktop-window-test PRIVATE deskwaifu_offscreen Qt6::Gui Qt6::OpenGL Qt6::WaylandClient LayerShellQt::Interface OpenGL::EGL ${CMAKE_DL_LIBS})
target_link_options(qt-desktop-window-test PRIVATE -Wl,--export-dynamic-symbol=eglSwapBuffers)
add_test(NAME qt-window-reject-platform COMMAND qt-desktop-window-test --reject-platform)
set_tests_properties(qt-window-reject-platform PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
find_program(WAYLAND_SCANNER wayland-scanner)
if(NOT WAYLAND_SCANNER)
  message(FATAL_ERROR "The desktop regression fixture requires wayland-scanner")
endif()
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/fake-input-client.h ${CMAKE_CURRENT_BINARY_DIR}/fake-input-protocol.c
  COMMAND ${WAYLAND_SCANNER} client-header ${TEST_SCRIPTS}/fake-input.xml ${CMAKE_CURRENT_BINARY_DIR}/fake-input-client.h
  COMMAND ${WAYLAND_SCANNER} private-code ${TEST_SCRIPTS}/fake-input.xml ${CMAKE_CURRENT_BINARY_DIR}/fake-input-protocol.c
  DEPENDS ${TEST_SCRIPTS}/fake-input.xml)
add_executable(click-through-test ClickThroughTest.cpp ${PRIVATE_ROOT}/processor/interaction/window/QtDesktopWindow.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/fake-input-client.h ${CMAKE_CURRENT_BINARY_DIR}/fake-input-protocol.c)
target_include_directories(click-through-test PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(click-through-test PRIVATE deskwaifu_offscreen Qt6::WaylandClient LayerShellQt::Interface PkgConfig::WAYLAND_CLIENT)
option(DESKWAIFU_QT_LIVE_TEST "Run Qt window integration test on the selected Wayland display" OFF)
if(DESKWAIFU_QT_LIVE_TEST)
  add_test(NAME gpu-transfer COMMAND gpu-transfer-test)
  add_test(NAME qt-desktop-window COMMAND qt-desktop-window-test)
  set_tests_properties(gpu-transfer qt-desktop-window PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
endif()
add_executable(kwin-cursor-tracker-test KWinCursorTrackerTest.cpp)
target_link_libraries(kwin-cursor-tracker-test PRIVATE deskwaifu_kwin_cursor Threads::Threads)
add_dependencies(kwin-cursor-tracker-test deskwaifu-cursor-guard)
find_program(DBUS_RUN_SESSION dbus-run-session)
if(DBUS_RUN_SESSION)
  add_test(NAME kwin-cursor-tracker COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:kwin-cursor-tracker-test>)
endif()
find_program(NODE_EXECUTABLE NAMES node nodejs)
if(NODE_EXECUTABLE)
  add_test(NAME kwin-cursor-script COMMAND ${NODE_EXECUTABLE}
    ${TEST_SCRIPTS}/test_kwin_cursor_script.js $<TARGET_FILE:kwin-cursor-tracker-test>)
endif()
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND AND DBUS_RUN_SESSION)
  add_test(NAME kwin-cursor-locale COMMAND ${Python3_EXECUTABLE}
    ${TEST_SCRIPTS}/test_kwin_cursor_locale.py $<TARGET_FILE:kwin-cursor-tracker-test>
    ${DBUS_RUN_SESSION} ${CMAKE_CURRENT_BINARY_DIR})
  set_tests_properties(kwin-cursor-locale PROPERTIES SKIP_RETURN_CODE 77)
endif()

set(CONFIG_SOURCE ${PRIVATE_ROOT}/processor/interaction/config/LAppConfig.cpp)
set(INPUT_SOURCE ${PRIVATE_ROOT}/processor/interaction/InputController.cpp)
add_executable(transform-editor-test TransformEditorTest.cpp ${CONFIG_SOURCE} ${INPUT_SOURCE})
add_test(NAME transform-editor COMMAND transform-editor-test)
add_test(NAME config-temp-directory COMMAND ${CMAKE_COMMAND}
  -DTEST_TEMP_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tmp -DCONFIG_TEST=$<TARGET_FILE:config-store-test>
  -DEDITOR_TEST=$<TARGET_FILE:transform-editor-test> -P ${TEST_SCRIPTS}/CheckPortableTemporaryDirectory.cmake)
foreach(pair "lapp-config|LAppConfig" "model-region|ModelRegion" "hover-opacity|HoverOpacity"
    "hover-opacity-settings|HoverOpacitySettings" "mouse-look-settings|MouseLookSettings"
    "render-device-settings|RenderDeviceSettings")
  string(REPLACE "|" ";" pair "${pair}")
  list(GET pair 0 name)
  list(GET pair 1 source)
  add_executable(${name}-test ${source}Test.cpp ${CONFIG_SOURCE})
  add_test(NAME ${name} COMMAND ${name}-test)
endforeach()
add_executable(model-region-render-test ModelRegionRenderTest.cpp ${PRIVATE_ROOT}/processor/model/ModelFrameRegion.cpp)
target_link_libraries(model-region-render-test PRIVATE Framework glew_s OpenGL::EGL)
add_test(NAME model-region-render COMMAND model-region-render-test)
set_tests_properties(model-region-render PROPERTIES ENVIRONMENT "EGL_PLATFORM=surfaceless" SKIP_RETURN_CODE 77)
add_executable(lapp-sprite-test LAppSpriteTest.cpp ${PRIVATE_ROOT}/processor/model/LAppSprite.cpp ${COMMON_SRC_PATH}/LAppSprite_Common.cpp)
target_include_directories(lapp-sprite-test PRIVATE ${PRIVATE_ROOT}/processor/model ${COMMON_SRC_PATH})
target_link_libraries(lapp-sprite-test PRIVATE Framework glew_s OpenGL::EGL)
add_test(NAME lapp-sprite COMMAND lapp-sprite-test)
set_tests_properties(lapp-sprite PROPERTIES ENVIRONMENT "EGL_PLATFORM=surfaceless" SKIP_RETURN_CODE 77)
add_executable(edit-input-region-test EditInputRegionTest.cpp ${CONFIG_SOURCE} ${INPUT_SOURCE})
target_link_libraries(edit-input-region-test PRIVATE Qt6::Gui)
add_test(NAME edit-input-region COMMAND edit-input-region-test)
add_executable(failure-diagnostics-test FailureDiagnosticsTest.cpp)
add_test(NAME failure-diagnostics COMMAND failure-diagnostics-test)
add_executable(model-catalog-test ModelCatalogTest.cpp)
target_link_libraries(model-catalog-test PRIVATE deskwaifu_catalog)
add_test(NAME model-catalog COMMAND model-catalog-test)
add_executable(model-cache-test ModelCacheTest.cpp)
add_test(NAME model-cache COMMAND model-cache-test)
add_executable(model-session-test ModelSessionTest.cpp)
add_test(NAME model-session COMMAND model-session-test)
add_test(NAME model-hotkeys-header COMMAND ${CMAKE_COMMAND} -DAPP_SOURCE=${PRIVATE_ROOT}
  -DSDK_SOURCE=${CMAKE_SOURCE_DIR}/sdk
  -DCXX_COMPILER=${CMAKE_CXX_COMPILER} -DFIXTURE_SOURCE=${CMAKE_CURRENT_BINARY_DIR}/hotkeys-header
  -P ${TEST_SCRIPTS}/CheckModelHotkeysHeader.cmake)
add_executable(input-controller-test InputControllerTest.cpp)
target_link_libraries(input-controller-test PRIVATE deskwaifu_input)
add_test(NAME input-controller COMMAND input-controller-test)
add_executable(cursor-look-test CursorLookTest.cpp)
target_link_libraries(cursor-look-test PRIVATE deskwaifu_input)
add_test(NAME cursor-look COMMAND cursor-look-test)
add_executable(model-gaze-test ModelGazeTest.cpp
  ${PRIVATE_ROOT}/processor/model/LAppModel.cpp ${PRIVATE_ROOT}/processor/model/ModelHotkeys.cpp
  ${PRIVATE_ROOT}/processor/model/LAppPal.cpp ${PRIVATE_ROOT}/processor/model/LAppDefine.cpp
  ${PRIVATE_ROOT}/processor/model/LAppTextureManager.cpp
  ${COMMON_SRC_PATH}/LAppAllocator_Common.cpp ${COMMON_SRC_PATH}/LAppModel_Common.cpp
  ${COMMON_SRC_PATH}/LAppTextureManager_Common.cpp ${COMMON_SRC_PATH}/LAppWavFileHandler_Common.cpp)
target_include_directories(model-gaze-test PRIVATE ${PRIVATE_ROOT}/processor/model ${COMMON_SRC_PATH} ${STB_PATH} ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(model-gaze-test PRIVATE Framework OpenGL::EGL deskwaifu_input)
target_compile_definitions(model-gaze-test PRIVATE SDK_RESOURCES="${SDK_ROOT_PATH}/Samples/Resources")
add_test(NAME model-gaze COMMAND model-gaze-test)
set_tests_properties(model-gaze PROPERTIES ENVIRONMENT "EGL_PLATFORM=surfaceless" SKIP_RETURN_CODE 77)

add_executable(delegate-keyboard-test DelegateKeyboardTest.cpp)
target_link_libraries(delegate-keyboard-test PRIVATE deskwaifu_application OpenGL::EGL)
target_compile_definitions(delegate-keyboard-test PRIVATE SDK_RESOURCES="${SDK_ROOT_PATH}/Samples/Resources")
target_link_options(delegate-keyboard-test PRIVATE -Wl,--wrap=_ZN12ModelHotkeys7TriggerERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERS5_)
add_test(NAME delegate-keyboard COMMAND delegate-keyboard-test)
set_tests_properties(delegate-keyboard PROPERTIES ENVIRONMENT "EGL_PLATFORM=surfaceless" TIMEOUT 30)
# Compile the real desktop plugin entry with only its window boundary replaced.
# The platform integration loader links neither this module nor desktop's core.
add_library(desktop-keyboard-fixture MODULE ../DesktopPlugin.cpp DelegatePluginWindow.cpp)
deskwaifu_domain_test(desktop-keyboard-fixture desktop)
target_link_libraries(desktop-keyboard-fixture PRIVATE deskwaifu_application OpenGL::EGL)
set_target_properties(desktop-keyboard-fixture PROPERTIES PREFIX "" OUTPUT_NAME desktop)
add_executable(application-service-test ApplicationServiceTest.cpp)
add_test(NAME application-service COMMAND application-service-test)
add_executable(external-control-test ExternalControlTest.cpp)
add_test(NAME external-control COMMAND external-control-test)
add_executable(model-render-thread-test ModelRenderThreadTest.cpp)
target_link_libraries(model-render-thread-test PRIVATE deskwaifu_application)
target_link_options(model-render-thread-test PRIVATE
  -Wl,--wrap=_ZNK17LAppLive2DManager6OnDragEff -Wl,--wrap=_ZN9LAppModel6UpdateERK15ModelFrameInput
  -Wl,--wrap=glGenTextures -Wl,--wrap=glDeleteTextures)
target_compile_definitions(model-render-thread-test PRIVATE SDK_RESOURCES="${SDK_ROOT_PATH}/Samples/Resources")
add_test(NAME model-render-thread COMMAND model-render-thread-test)
add_test(NAME model-shutdown-context COMMAND model-render-thread-test shutdown-context)
set_tests_properties(model-shutdown-context model-render-thread PROPERTIES ENVIRONMENT "EGL_PLATFORM=surfaceless" TIMEOUT 30)
add_executable(model-pointer-test ModelPointerTest.cpp)
target_include_directories(model-pointer-test PRIVATE ${PRIVATE_ROOT}/processor/model ${COMMON_SRC_PATH} ${STB_PATH} ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(model-pointer-test PRIVATE deskwaifu_application OpenGL::EGL)
target_compile_definitions(model-pointer-test PRIVATE SDK_RESOURCES="${SDK_ROOT_PATH}/Samples/Resources")
target_link_options(model-pointer-test PRIVATE
  -Wl,--wrap=_ZN7LAppPal10PrintLogLnEPKcz -Wl,--wrap=_ZN9LAppModel19SetRandomExpressionEv
  -Wl,--wrap=_ZN17KWinCursorTracker5StartEv
  -Wl,--wrap=_ZN17KWinCursorTracker4PollERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE
  -Wl,--wrap=_ZNK17KWinCursorTracker13LocalPositionEiiRdS0_)
foreach(scenario release transform drag state cursor)
  add_test(NAME model-pointer-${scenario} COMMAND model-pointer-test ${scenario})
  set_tests_properties(model-pointer-${scenario} PROPERTIES ENVIRONMENT "EGL_PLATFORM=surfaceless" SKIP_RETURN_CODE 77 TIMEOUT 30)
endforeach()
add_executable(texture-residency-test TextureResidencyTest.cpp)
target_include_directories(texture-residency-test PRIVATE ${GLEW_PATH}/include ${FRAMEWORK_PATH}/src ${CORE_PATH}/include ${COMMON_SRC_PATH})
add_test(NAME texture-residency COMMAND texture-residency-test)
add_executable(output-controller-test OutputControllerTest.cpp)
target_link_libraries(output-controller-test PRIVATE deskwaifu_output)
add_test(NAME output-controller COMMAND output-controller-test)
add_executable(output-settings-test OutputSettingsTest.cpp ${CONFIG_SOURCE} ${PRIVATE_ROOT}/processor/interaction/window/OutputController.cpp)
target_link_options(output-settings-test PRIVATE -Wl,--wrap=mkstemp)
add_test(NAME output-settings COMMAND output-settings-test)
add_executable(compositor-adapter-test CompositorAdapterTest.cpp)
target_link_libraries(compositor-adapter-test PRIVATE deskwaifu_compositor)
target_link_options(compositor-adapter-test PRIVATE -Wl,--wrap=waitpid)
add_test(NAME compositor-adapter COMMAND compositor-adapter-test)
add_executable(signal-processor-test SignalProcessorTest.cpp)
target_link_libraries(signal-processor-test PRIVATE deskwaifu_application)
add_test(NAME signal-processor COMMAND signal-processor-test)
add_executable(interaction-processor-test InteractionProcessorTest.cpp)
target_link_libraries(interaction-processor-test PRIVATE deskwaifu_application)
add_test(NAME interaction-processor COMMAND interaction-processor-test)
add_test(NAME interaction-opacity COMMAND interaction-processor-test opacity)
set_tests_properties(interaction-opacity PROPERTIES TIMEOUT 10 LABELS "contract;interaction")
set_tests_properties(interaction-processor PROPERTIES TIMEOUT 30)
add_executable(desktop-ownership-test DesktopOwnershipTest.cpp)
target_link_libraries(desktop-ownership-test PRIVATE deskwaifu_application)
add_test(NAME desktop-ownership COMMAND desktop-ownership-test)
set_tests_properties(desktop-ownership PROPERTIES TIMEOUT 30)

if(DESKWAIFU_BUILD_CONTROL)
  find_package(Qt6 REQUIRED COMPONENTS Test)
  add_library(desktop-settings-fixture MODULE DesktopSettingsFixture.cpp ${CONFIG_SOURCE} ${INPUT_SOURCE})
  deskwaifu_domain_test(desktop-settings-fixture desktop)
  set_target_properties(desktop-settings-fixture PROPERTIES PREFIX "")
  qt_add_executable(model-hotkeys-test ModelHotkeysTest.cpp ${PRIVATE_ROOT}/processor/model/ModelHotkeys.cpp ${COMMON_SRC_PATH}/LAppAllocator_Common.cpp)
  target_include_directories(model-hotkeys-test PRIVATE ${COMMON_SRC_PATH})
  target_compile_definitions(model-hotkeys-test PRIVATE SDK_RESOURCES="${SDK_ROOT_PATH}/Samples/Resources")
  target_link_libraries(model-hotkeys-test PRIVATE Qt6::Test Framework)
  set_target_properties(model-hotkeys-test PROPERTIES AUTOMOC ON)
  add_test(NAME model-hotkeys COMMAND model-hotkeys-test)
  qt_add_executable(model-transform-test ModelTransformTest.cpp)
  target_link_libraries(model-transform-test PRIVATE Qt6::Test Framework)
  set_target_properties(model-transform-test PROPERTIES AUTOMOC ON)
  add_test(NAME model-transform COMMAND model-transform-test)
  qt_add_executable(model-keyboard-test ModelKeyboardTest.cpp)
  target_link_libraries(model-keyboard-test PRIVATE Qt6::Test)
  set_target_properties(model-keyboard-test PROPERTIES AUTOMOC ON)
  add_test(NAME model-keyboard COMMAND model-keyboard-test)
endif()

add_executable(qt-desktop-window-run-test QtDesktopWindowRunTest.cpp)
target_link_libraries(qt-desktop-window-run-test PRIVATE deskwaifu_offscreen Qt6::WaylandClient LayerShellQt::Interface)
add_test(NAME qt-desktop-window-run COMMAND qt-desktop-window-run-test)
set_tests_properties(qt-desktop-window-run PROPERTIES TIMEOUT 10 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

get_property(test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(target IN LISTS test_targets)
  get_target_property(kind ${target} TYPE)
  if(kind STREQUAL "EXECUTABLE")
    deskwaifu_domain_test(${target} desktop)
  endif()
endforeach()

add_subdirectory(commands)
