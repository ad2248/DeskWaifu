# Controlling an existing desktop must not require building its renderer.
foreach(command toggle --toggle focus --focus)
  configure_file(ControlCommand.sh "${CMAKE_BINARY_DIR}/plugins/desktop/commands/${command}"
    COPYONLY FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
  install(PROGRAMS ControlCommand.sh
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/deskwaifu/plugins/desktop/commands RENAME "${command}" COMPONENT DeskWaifu)
endforeach()
if(BUILD_TESTING)
  add_test(NAME desktop-cli COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/DesktopControlTest.py $<TARGET_FILE:deskwaifu> ${CMAKE_BINARY_DIR}/plugins)
  set_tests_properties(desktop-cli PROPERTIES TIMEOUT 15 LABELS "contract;regression")
endif()
