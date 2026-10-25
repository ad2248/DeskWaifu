find_package(Threads REQUIRED)

add_executable(plugin-event-test EventTest.cpp)
target_link_libraries(plugin-event-test PRIVATE deskwaifu_plugin_sdk Threads::Threads)
add_test(NAME plugin-events COMMAND plugin-event-test)

# Compile the existing Legacy contract unchanged as well.
add_executable(plugin-legacy-event-test ../../processor/tests/ProcessorEventLoopTest.cpp)
target_link_libraries(plugin-legacy-event-test PRIVATE deskwaifu_events Threads::Threads)
add_test(NAME plugin-legacy-events COMMAND plugin-legacy-event-test)

foreach(kind good consumer init_fail start_fail bad_abi bad_build_abi bad_entry)
  add_library(plugin-fixture-${kind} MODULE Fixture.cpp)
  target_link_libraries(plugin-fixture-${kind} PRIVATE deskwaifu_plugin_sdk)
  target_compile_definitions(plugin-fixture-${kind} PRIVATE FIXTURE_${kind})
  set_target_properties(plugin-fixture-${kind} PROPERTIES PREFIX "" OUTPUT_NAME "${kind}")
endforeach()

add_executable(plugin-manager-test ManagerTest.cpp)
target_link_libraries(plugin-manager-test PRIVATE deskwaifu_plugin_host Threads::Threads)
add_test(NAME plugin-manager COMMAND plugin-manager-test
  $<TARGET_FILE:plugin-fixture-good>
  $<TARGET_FILE:plugin-fixture-consumer>
  $<TARGET_FILE:plugin-fixture-init_fail>
  $<TARGET_FILE:plugin-fixture-start_fail>
  $<TARGET_FILE:plugin-fixture-bad_abi>
  $<TARGET_FILE:plugin-fixture-bad_build_abi>
  $<TARGET_FILE:plugin-fixture-bad_entry>)
set_tests_properties(plugin-events plugin-legacy-events plugin-manager PROPERTIES TIMEOUT 30)
