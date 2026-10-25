find_program(SCREENSHOT_DBUS_RUN_SESSION dbus-run-session)
if(SCREENSHOT_DBUS_RUN_SESSION)
  add_executable(screenshot-service-test ScreenshotServiceTest.cpp)
  set_target_properties(screenshot-service-test PROPERTIES AUTOMOC ON)
  target_link_libraries(screenshot-service-test PRIVATE deskwaifu_screenshot Qt6::DBus Qt6::Concurrent)
  add_test(NAME screenshot-service
    COMMAND ${SCREENSHOT_DBUS_RUN_SESSION} -- $<TARGET_FILE:screenshot-service-test>)
  set_tests_properties(screenshot-service PROPERTIES TIMEOUT 30
    ENVIRONMENT "DESKWAIFU_SCREENSHOT_TEST_BUS=1")
endif()
