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

add_executable(json-command-test JsonCommandTest.cpp)
target_include_directories(json-command-test PRIVATE ../src)
add_test(NAME json-command COMMAND json-command-test)

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

add_executable(output-controller-test OutputControllerTest.cpp)
target_link_libraries(output-controller-test PRIVATE deskwaifu_output)
add_test(NAME output-controller COMMAND output-controller-test)

add_executable(compositor-adapter-test CompositorAdapterTest.cpp)
target_link_libraries(compositor-adapter-test PRIVATE deskwaifu_compositor)
add_test(NAME compositor-adapter COMMAND compositor-adapter-test)

if (NOT DESKWAIFU_BUILD_CONTROL)
  return()
endif ()

find_package(Qt6 REQUIRED COMPONENTS Test Network Widgets)

qt_add_executable(deskwaifu-client-test
  DeskWaifuClientTest.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../control/DeskWaifuClient.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../control/DeskWaifuClient.hpp
)

target_include_directories(deskwaifu-client-test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../control
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

qt_add_executable(model-keyboard-test ModelKeyboardTest.cpp)
target_include_directories(model-keyboard-test PRIVATE ../src ${FRAMEWORK_PATH}/src ${CORE_PATH}/include)
target_link_libraries(model-keyboard-test PRIVATE Qt6::Test)
set_target_properties(model-keyboard-test PROPERTIES AUTOMOC ON)
add_test(NAME model-keyboard COMMAND model-keyboard-test)
