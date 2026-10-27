find_package(Threads REQUIRED)

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
set_tests_properties(plugin-manager PROPERTIES TIMEOUT 30)

if(DESKWAIFU_BUILD_GUI_PLUGIN_SDK)
  foreach(kind good dual backend_only init_fail start_fail init_throw start_throw duplicate bad_abi bad_build bad_qt factory_throw validation)
    add_library(gui-plugin-fixture-${kind} MODULE GuiFixture.cpp)
    target_link_libraries(gui-plugin-fixture-${kind} PRIVATE deskwaifu_gui_plugin_sdk)
    target_compile_definitions(gui-plugin-fixture-${kind} PRIVATE FIXTURE_${kind})
    set_target_properties(gui-plugin-fixture-${kind} PROPERTIES PREFIX "" OUTPUT_NAME "gui_${kind}")
  endforeach()
  add_executable(gui-plugin-manager-test GuiManagerTest.cpp)
  target_link_libraries(gui-plugin-manager-test PRIVATE deskwaifu_gui_plugin_host deskwaifu_plugin_host Threads::Threads)
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

  foreach(kind good init_fail start_fail release destroyed)
    add_library(gui-transport-fixture-${kind} MODULE GuiTransportFixture.cpp)
    target_link_libraries(gui-transport-fixture-${kind} PRIVATE deskwaifu_gui_plugin_sdk)
    target_compile_definitions(gui-transport-fixture-${kind} PRIVATE FIXTURE_${kind})
    set_target_properties(gui-transport-fixture-${kind} PROPERTIES PREFIX "" OUTPUT_NAME "transport_${kind}")
  endforeach()
  add_executable(gui-transport-manager-test GuiTransportManagerTest.cpp)
  target_link_libraries(gui-transport-manager-test PRIVATE deskwaifu_gui_plugin_host)
  add_test(NAME gui-transport-manager COMMAND gui-transport-manager-test
    $<TARGET_FILE:gui-transport-fixture-good>
    $<TARGET_FILE:gui-transport-fixture-init_fail>
    $<TARGET_FILE:gui-transport-fixture-start_fail>
    $<TARGET_FILE:gui-transport-fixture-release>
    $<TARGET_FILE:gui-transport-fixture-destroyed>)
  set_tests_properties(gui-transport-manager PROPERTIES TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
