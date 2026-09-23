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
target_link_libraries(model-transform-test PRIVATE Qt6::Test)
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
