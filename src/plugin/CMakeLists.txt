cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(deskwaifu_plugin_sdk LANGUAGES CXX)
  include(CTest)
endif()
include(GNUInstallDirs)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core)

# Standalone tests need no Cubism or domain libraries. In the application the
# parent defines this same SHARED target before adding this directory.
if(NOT TARGET deskwaifu_events)
  add_library(deskwaifu_events SHARED ../event/ProcessorEventLoop.cpp)
  target_include_directories(deskwaifu_events PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/deskwaifu>)
  target_link_libraries(deskwaifu_events PUBLIC Qt6::Core)
  target_compile_features(deskwaifu_events PUBLIC cxx_std_14)
  set_target_properties(deskwaifu_events PROPERTIES VERSION 1.0 SOVERSION 1)
endif()

add_library(deskwaifu_plugin_sdk SHARED PluginContext.cpp ConfigJson.cpp GuiHostServices.hpp)
set_target_properties(deskwaifu_plugin_sdk PROPERTIES AUTOMOC ON)
target_include_directories(deskwaifu_plugin_sdk PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/deskwaifu>)
target_link_libraries(deskwaifu_plugin_sdk PUBLIC deskwaifu_events Qt6::Core)
target_compile_features(deskwaifu_plugin_sdk PUBLIC cxx_std_14)
set_target_properties(deskwaifu_plugin_sdk PROPERTIES VERSION 1.0 SOVERSION 1)
set_target_properties(deskwaifu_events PROPERTIES EXPORT_NAME events)
set_target_properties(deskwaifu_plugin_sdk PROPERTIES EXPORT_NAME plugin_sdk INSTALL_RPATH "$ORIGIN")
install(TARGETS deskwaifu_events deskwaifu_plugin_sdk EXPORT DeskWaifuPluginSDKTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT DeskWaifu NAMELINK_COMPONENT DeskWaifuSDK)
install(FILES PluginApi.hpp PluginContext.hpp ConfigJson.hpp GuiHostServices.hpp GuiPluginApi.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/deskwaifu/plugin COMPONENT DeskWaifuSDK)
install(DIRECTORY ../event/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/deskwaifu/event
  COMPONENT DeskWaifuSDK FILES_MATCHING PATTERN "*.hpp")
install(FILES ../processor/Processor.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/deskwaifu/processor COMPONENT DeskWaifuSDK)
install(EXPORT DeskWaifuPluginSDKTargets NAMESPACE DeskWaifu::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DeskWaifuPluginSDK COMPONENT DeskWaifuSDK)
install(FILES DeskWaifuPluginSDKConfig.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DeskWaifuPluginSDK COMPONENT DeskWaifuSDK)

add_library(deskwaifu_plugin_host STATIC PluginManager.cpp)
target_link_libraries(deskwaifu_plugin_host PUBLIC deskwaifu_plugin_sdk)
target_compile_features(deskwaifu_plugin_host PUBLIC cxx_std_14)

option(DESKWAIFU_BUILD_GUI_PLUGIN_SDK "Build the optional QtWidgets GUI plugin host" OFF)
if(DESKWAIFU_BUILD_GUI_PLUGIN_SDK)
  find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets)
  add_library(deskwaifu_gui_plugin_sdk SHARED GuiPluginContext.cpp GuiPluginManager.cpp)
  target_link_libraries(deskwaifu_gui_plugin_sdk PUBLIC deskwaifu_plugin_sdk Qt6::Widgets)
  target_compile_features(deskwaifu_gui_plugin_sdk PUBLIC cxx_std_14)
  set_target_properties(deskwaifu_gui_plugin_sdk PROPERTIES VERSION 1.0 SOVERSION 1
    EXPORT_NAME gui_plugin_sdk INSTALL_RPATH "$ORIGIN")
  install(TARGETS deskwaifu_gui_plugin_sdk EXPORT DeskWaifuGuiPluginSDKTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT DeskWaifu NAMELINK_COMPONENT DeskWaifuSDK)
  install(FILES GuiPluginContext.hpp GuiPluginManager.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/deskwaifu/plugin COMPONENT DeskWaifuSDK)
  install(EXPORT DeskWaifuGuiPluginSDKTargets NAMESPACE DeskWaifu::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DeskWaifuPluginSDK COMPONENT DeskWaifuSDK)
endif()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
