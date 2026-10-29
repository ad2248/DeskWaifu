find_package(Qt6 6.5 REQUIRED COMPONENTS Gui)
add_subdirectory(plugin)
add_executable(deskwaifu main.cpp HostConfig.cpp)
target_link_libraries(deskwaifu PRIVATE deskwaifu_plugin_host Qt6::Gui)
target_compile_definitions(deskwaifu PRIVATE
  DESKWAIFU_PLUGIN_DIR="${CMAKE_INSTALL_FULL_LIBDIR}/deskwaifu/plugins")
set_target_properties(deskwaifu PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
install(TARGETS deskwaifu RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DeskWaifu)
