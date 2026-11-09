# Exercise the production adapter; only its model/window runtime is replaced.
add_library(desktop-commands-fixture MODULE ../../DesktopPlugin.cpp DesktopCommandsRuntime.cpp)
target_include_directories(desktop-commands-fixture BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/stub")
target_link_libraries(desktop-commands-fixture PRIVATE deskwaifu_plugin_sdk)
set_target_properties(desktop-commands-fixture PROPERTIES PREFIX "" OUTPUT_NAME desktop)
