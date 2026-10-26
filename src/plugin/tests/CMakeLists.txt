find_package(Threads REQUIRED)

add_executable(plugin-event-test EventTest.cpp)
target_link_libraries(plugin-event-test PRIVATE deskwaifu_plugin_sdk Threads::Threads)
add_test(NAME plugin-events COMMAND plugin-event-test)
add_executable(plugin-timeout-flood-test TimeoutFloodTest.cpp)
target_link_libraries(plugin-timeout-flood-test PRIVATE deskwaifu_plugin_sdk)
add_test(NAME plugin-timeout-flood COMMAND plugin-timeout-flood-test)
set_tests_properties(plugin-timeout-flood PROPERTIES TIMEOUT 5)

# Compile the existing Legacy contract unchanged as well.
add_executable(plugin-legacy-event-test ../../processor/tests/ProcessorEventLoopTest.cpp)
target_link_libraries(plugin-legacy-event-test PRIVATE deskwaifu_events Threads::Threads)
add_test(NAME plugin-legacy-events COMMAND plugin-legacy-event-test)

foreach(kind good consumer init_fail start_fail bad_abi bad_build_abi bad_entry bad_debug_abi)
  add_library(plugin-fixture-${kind} MODULE Fixture.cpp)
  target_link_libraries(plugin-fixture-${kind} PRIVATE deskwaifu_plugin_sdk)
  target_compile_definitions(plugin-fixture-${kind} PRIVATE FIXTURE_${kind})
  set_target_properties(plugin-fixture-${kind} PROPERTIES PREFIX "" OUTPUT_NAME "${kind}")
endforeach()
target_compile_definitions(plugin-fixture-bad_debug_abi PRIVATE _GLIBCXX_DEBUG)

add_executable(plugin-manager-test ManagerTest.cpp)
target_link_libraries(plugin-manager-test PRIVATE deskwaifu_plugin_host Threads::Threads)
add_test(NAME plugin-manager COMMAND plugin-manager-test
  $<TARGET_FILE:plugin-fixture-good>
  $<TARGET_FILE:plugin-fixture-consumer>
  $<TARGET_FILE:plugin-fixture-init_fail>
  $<TARGET_FILE:plugin-fixture-start_fail>
  $<TARGET_FILE:plugin-fixture-bad_abi>
  $<TARGET_FILE:plugin-fixture-bad_build_abi>
  $<TARGET_FILE:plugin-fixture-bad_entry>
  $<TARGET_FILE:plugin-fixture-bad_debug_abi>)
set_tests_properties(plugin-events plugin-legacy-events plugin-manager PROPERTIES TIMEOUT 30)

if(DESKWAIFU_BUILD_GUI_PLUGIN_SDK)
  foreach(kind good dual backend_only init_fail start_fail init_throw start_throw duplicate bad_abi bad_build bad_qt factory_throw validation)
    add_library(gui-plugin-fixture-${kind} MODULE GuiFixture.cpp)
    target_link_libraries(gui-plugin-fixture-${kind} PRIVATE deskwaifu_gui_plugin_sdk)
    target_compile_definitions(gui-plugin-fixture-${kind} PRIVATE FIXTURE_${kind})
    set_target_properties(gui-plugin-fixture-${kind} PROPERTIES PREFIX "" OUTPUT_NAME "gui_${kind}")
  endforeach()
  add_executable(gui-plugin-manager-test GuiManagerTest.cpp)
  target_link_libraries(gui-plugin-manager-test PRIVATE deskwaifu_gui_plugin_sdk deskwaifu_plugin_host Threads::Threads)
  add_test(NAME gui-plugin-manager COMMAND gui-plugin-manager-test
    $<TARGET_FILE:gui-plugin-fixture-good>
    $<TARGET_FILE:gui-plugin-fixture-dual>
    $<TARGET_FILE:gui-plugin-fixture-backend_only>
    $<TARGET_FILE:gui-plugin-fixture-init_fail>
    $<TARGET_FILE:gui-plugin-fixture-start_fail>
    $<TARGET_FILE:gui-plugin-fixture-init_throw>
    $<TARGET_FILE:gui-plugin-fixture-start_throw>
    $<TARGET_FILE:gui-plugin-fixture-duplicate>
    $<TARGET_FILE:gui-plugin-fixture-bad_abi>
    $<TARGET_FILE:gui-plugin-fixture-bad_build>
    $<TARGET_FILE:gui-plugin-fixture-bad_qt>
    $<TARGET_FILE:gui-plugin-fixture-factory_throw>
    $<TARGET_FILE:gui-plugin-fixture-validation>)
  set_tests_properties(gui-plugin-manager PROPERTIES TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
