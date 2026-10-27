cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(deskwaifu_plugin_sdk LANGUAGES CXX)
  include(CTest)
endif()
include(GNUInstallDirs)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core)
option(DESKWAIFU_BUILD_GUI_PLUGIN_SDK "Build the optional QtWidgets GUI plugin SDK" OFF)

include(cmake/CheckOwnership.cmake)
add_custom_target(deskwaifu_sdk_ownership ALL
  COMMAND ${CMAKE_COMMAND} -DSDK_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckOwnership.cmake
  COMMENT "Checking SDK ownership boundaries")

add_library(deskwaifu_events SHARED event/ProcessorEventLoop.cpp)
add_library(deskwaifu_contracts STATIC contracts/ConfigStore.cpp)
add_library(deskwaifu_plugin_sdk SHARED plugin/PluginContext.cpp plugin/ConfigJson.cpp plugin/GuiHostServices.hpp)
set_target_properties(deskwaifu_contracts PROPERTIES POSITION_INDEPENDENT_CODE ON EXPORT_NAME contracts)
set_target_properties(deskwaifu_plugin_sdk PROPERTIES AUTOMOC ON)
foreach(target deskwaifu_events deskwaifu_contracts deskwaifu_plugin_sdk)
  target_include_directories(${target} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/deskwaifu>)
  target_link_libraries(${target} PUBLIC Qt6::Core)
  target_compile_features(${target} PUBLIC cxx_std_14)
  add_dependencies(${target} deskwaifu_sdk_ownership)
endforeach()
target_link_libraries(deskwaifu_plugin_sdk PUBLIC deskwaifu_events deskwaifu_contracts)
set_target_properties(deskwaifu_events PROPERTIES VERSION 2.0 SOVERSION 2 EXPORT_NAME events)
set_target_properties(deskwaifu_plugin_sdk PROPERTIES VERSION 2.0 SOVERSION 2
  EXPORT_NAME plugin_sdk INSTALL_RPATH "$ORIGIN")

install(TARGETS deskwaifu_events deskwaifu_contracts deskwaifu_plugin_sdk EXPORT DeskWaifuPluginSDKTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT DeskWaifu NAMELINK_COMPONENT DeskWaifuSDK
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT DeskWaifuSDK)
install(DIRECTORY plugin event contracts processor
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/deskwaifu COMPONENT DeskWaifuSDK
  FILES_MATCHING PATTERN "*.hpp")
install(EXPORT DeskWaifuPluginSDKTargets NAMESPACE DeskWaifu::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DeskWaifuPluginSDK COMPONENT DeskWaifuSDK)
install(FILES DeskWaifuPluginSDKConfig.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DeskWaifuPluginSDK COMPONENT DeskWaifuSDK)

if(DESKWAIFU_BUILD_GUI_PLUGIN_SDK)
  find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets)
  add_library(deskwaifu_gui_plugin_sdk SHARED plugin/GuiPluginContext.cpp)
  target_link_libraries(deskwaifu_gui_plugin_sdk PUBLIC deskwaifu_plugin_sdk Qt6::Widgets)
  target_compile_features(deskwaifu_gui_plugin_sdk PUBLIC cxx_std_14)
  set_target_properties(deskwaifu_gui_plugin_sdk PROPERTIES VERSION 2.0 SOVERSION 2
    EXPORT_NAME gui_plugin_sdk INSTALL_RPATH "$ORIGIN")
  add_dependencies(deskwaifu_gui_plugin_sdk deskwaifu_sdk_ownership)
  install(TARGETS deskwaifu_gui_plugin_sdk EXPORT DeskWaifuGuiPluginSDKTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT DeskWaifu NAMELINK_COMPONENT DeskWaifuSDK)
  install(EXPORT DeskWaifuGuiPluginSDKTargets NAMESPACE DeskWaifu::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DeskWaifuPluginSDK COMPONENT DeskWaifuSDK)
endif()

if(BUILD_TESTING)
  # Repository contract tests belong to the platform; production SDK targets
  # never compile or link platform implementations.
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../platform/tests/sdk ${CMAKE_CURRENT_BINARY_DIR}/tests)
endif()
