cmake_minimum_required(VERSION 3.18)
project(dw_cubism LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
find_package(OpenGL REQUIRED COMPONENTS OpenGL EGL)

add_library(dw_glew STATIC "${GLEW}/src/glew.c")
target_include_directories(dw_glew PUBLIC "${GLEW}/include")
target_compile_definitions(dw_glew PRIVATE GLEW_EGL PUBLIC GLEW_STATIC GLEW_NO_GLU)
target_link_libraries(dw_glew PUBLIC OpenGL::OpenGL OpenGL::EGL)

set(FRAMEWORK_SOURCE OpenGL)
add_subdirectory("${SDK}/Framework" framework)
target_compile_definitions(Framework PUBLIC CSM_TARGET_LINUX_GL)
target_include_directories(Framework PUBLIC "${SDK}/Core/include")
target_link_libraries(Framework PUBLIC dw_glew)

add_library(dw_cubism STATIC offscreen.cpp model.cpp)
target_include_directories(dw_cubism PRIVATE "${GENERATED}" ../../../thirdParty/stb)
target_link_libraries(dw_cubism PRIVATE Framework)

if(LAYER_SHELL_XML)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client wayland-egl)
    pkg_get_variable(WAYLAND_PROTOCOL_DIR wayland-protocols pkgdatadir)
    find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
    set(XDG_SHELL_XML "${WAYLAND_PROTOCOL_DIR}/stable/xdg-shell/xdg-shell.xml")
    add_custom_command(
        OUTPUT "${GENERATED}/layer-shell.h" "${GENERATED}/layer-shell.c" "${GENERATED}/xdg-shell.c"
        COMMAND "${WAYLAND_SCANNER}" client-header "${LAYER_SHELL_XML}" "${GENERATED}/layer-shell.h"
        COMMAND "${WAYLAND_SCANNER}" private-code "${LAYER_SHELL_XML}" "${GENERATED}/layer-shell.c"
        COMMAND "${WAYLAND_SCANNER}" private-code "${XDG_SHELL_XML}" "${GENERATED}/xdg-shell.c"
        DEPENDS "${LAYER_SHELL_XML}" "${XDG_SHELL_XML}"
        VERBATIM)
    target_sources(dw_cubism PRIVATE window_probe.cpp "${GENERATED}/layer-shell.h"
        "${GENERATED}/layer-shell.c" "${GENERATED}/xdg-shell.c")
    target_link_libraries(dw_cubism PRIVATE PkgConfig::WAYLAND)
endif()
