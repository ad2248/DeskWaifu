find_package(Qt6 6.5 REQUIRED COMPONENTS Gui DBus Concurrent)
add_library(deskwaifu_screenshot STATIC
  IScreenshotService.hpp portal/PortalScreenshotService.hpp portal/PortalScreenshotService.cpp)
set_target_properties(deskwaifu_screenshot PROPERTIES AUTOMOC ON)
target_include_directories(deskwaifu_screenshot PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(deskwaifu_screenshot PUBLIC Qt6::Gui PRIVATE Qt6::DBus Qt6::Concurrent)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
