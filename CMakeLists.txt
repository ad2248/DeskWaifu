cmake_minimum_required(VERSION 3.25)
project(deskwaifu LANGUAGES C CXX)
include(GNUInstallDirs)
include(CTest)

option(DESKWAIFU_BUILD_RUST_RUNTIME "Build the Rust model runtime executable" ON)

if(DESKWAIFU_BUILD_RUST_RUNTIME)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  message(FATAL_ERROR "The Rust runtime build is supported only on Linux x86_64 (got ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR})")
endif()
set(CUBISM_SDK_ROOT "${PROJECT_SOURCE_DIR}/CubismSdkForNative-5-r.5" CACHE PATH "External Cubism SDK for Native source tree")
set(GLEW_PATH "${PROJECT_SOURCE_DIR}/thirdParty/glew" CACHE PATH "GLEW source directory")
find_program(CARGO_EXECUTABLE cargo)
if(NOT CARGO_EXECUTABLE)
  message(FATAL_ERROR "Cargo is required to build the Rust runtime (install Rust/Cargo or set -DDESKWAIFU_BUILD_RUST_RUNTIME=OFF)")
endif()

file(GLOB_RECURSE DESKWAIFU_RUST_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/rust/app/*.rs"
  "${CMAKE_CURRENT_SOURCE_DIR}/rust/app/Cargo.toml"
  "${CMAKE_CURRENT_SOURCE_DIR}/rust/app/Cargo.lock"
  "${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.lock"
  "${CMAKE_CURRENT_SOURCE_DIR}/rust/cubism-core/*.rs"
  "${CMAKE_CURRENT_SOURCE_DIR}/rust/cubism-core/Cargo.toml"
  "${CMAKE_CURRENT_SOURCE_DIR}/rust/cubism-render/src/*.rs"
  "${CMAKE_CURRENT_SOURCE_DIR}/rust/cubism-render/native/*"
  "${CMAKE_CURRENT_SOURCE_DIR}/rust/cubism-render/build.rs"
  "${CMAKE_CURRENT_SOURCE_DIR}/rust/cubism-render/Cargo.toml"
  "${CMAKE_CURRENT_SOURCE_DIR}/rust/overlay/src/*.rs"
  "${CMAKE_CURRENT_SOURCE_DIR}/rust/overlay/Cargo.toml"
  "${CMAKE_CURRENT_SOURCE_DIR}/rust/hotkey-domain/src/*.rs"
  "${CMAKE_CURRENT_SOURCE_DIR}/rust/hotkey-domain/Cargo.toml"
  "${CMAKE_CURRENT_SOURCE_DIR}/rust/control-domain/src/*.rs"
  "${CMAKE_CURRENT_SOURCE_DIR}/rust/control-domain/Cargo.toml"
  "${CMAKE_CURRENT_SOURCE_DIR}/rust/ipc/*.rs"
  "${CMAKE_CURRENT_SOURCE_DIR}/rust/ipc/Cargo.toml"
  "${CMAKE_CURRENT_SOURCE_DIR}/rust/model-domain/src/*.rs"
  "${CMAKE_CURRENT_SOURCE_DIR}/rust/model-domain/Cargo.toml")
set(DESKWAIFU_RUST_TARGET_DIR "${CMAKE_BINARY_DIR}/cargo-target")
set(DESKWAIFU_RUST_BINARY "${DESKWAIFU_RUST_TARGET_DIR}/release/deskwaifu-rust")
add_custom_command(OUTPUT "${DESKWAIFU_RUST_BINARY}"
  COMMAND ${CMAKE_COMMAND} -E env
    "CUBISM_SDK_ROOT=${CUBISM_SDK_ROOT}"
    "GLEW_ROOT=${GLEW_PATH}"
    "CARGO_TARGET_DIR=${DESKWAIFU_RUST_TARGET_DIR}"
    "${CARGO_EXECUTABLE}" build --locked --release --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/rust/app/Cargo.toml"
  DEPENDS ${DESKWAIFU_RUST_SOURCES}
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)
add_custom_target(deskwaifu-rust ALL DEPENDS "${DESKWAIFU_RUST_BINARY}")
install(PROGRAMS "${DESKWAIFU_RUST_BINARY}" DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT DeskWaifu)
endif()
if(BUILD_TESTING)
  include(cmake/TestEnvironment.cmake)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(DESKWAIFU_BUILD_CONTROL "Build the GUI platform and GUI plugins" ON)
option(DESKWAIFU_BUILD_GUI_PLUGIN_SDK "Build the optional GUI SDK" ${DESKWAIFU_BUILD_CONTROL})
option(DESKWAIFU_BUILD_DESKTOP_PLUGIN "Build the model/window/input plugin" ON)
option(DESKWAIFU_BUILD_PERCEPTION_PLUGIN "Build the image/OCR/screenshot plugin" ON)
option(DESKWAIFU_BUILD_OCR "Build the optional in-process OCR module" ON)
option(DESKWAIFU_BUILD_IPC_PLUGIN "Build the IPC transport plugin" ON)
option(DESKWAIFU_BUILD_SPEECH_PLUGIN "Build the speech plugin" ON)
option(DESKWAIFU_BUILD_VOICE_INPUT_PLUGIN "Build the voice input plugin" ON)
option(DESKWAIFU_BUILD_READING_CLIP_PLUGIN "Build the clipboard reading plugin" ON)
option(DESKWAIFU_TEST_GATE "Require the complete ordinary regression gate" OFF)
option(DESKWAIFU_ENABLE_SANITIZERS "Enable AddressSanitizer and UBSan" OFF)
option(DESKWAIFU_PACKAGE_TESTS "Install standalone runtime tests for split test packages" OFF)
if(DESKWAIFU_BUILD_CONTROL AND NOT DESKWAIFU_BUILD_GUI_PLUGIN_SDK)
  message(FATAL_ERROR "The GUI platform requires the GUI SDK")
endif()
if(DESKWAIFU_ENABLE_SANITIZERS)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
if(DESKWAIFU_TEST_GATE)
  foreach(option BUILD_TESTING DESKWAIFU_BUILD_CONTROL DESKWAIFU_BUILD_DESKTOP_PLUGIN
      DESKWAIFU_BUILD_PERCEPTION_PLUGIN DESKWAIFU_BUILD_IPC_PLUGIN
      DESKWAIFU_BUILD_SPEECH_PLUGIN DESKWAIFU_BUILD_READING_CLIP_PLUGIN
      DESKWAIFU_BUILD_VOICE_INPUT_PLUGIN)
    if(NOT ${option})
      message(FATAL_ERROR "The complete regression gate requires ${option}")
    endif()
  endforeach()
  find_program(DBUS_RUN_SESSION dbus-run-session REQUIRED)
  find_program(NODE_EXECUTABLE NAMES node nodejs REQUIRED)
endif()

include(cmake/PluginBoundaries.cmake)
add_subdirectory(sdk)
add_subdirectory(platform)
add_subdirectory(plugins)
if(DESKWAIFU_BUILD_OCR)
  add_subdirectory(plugins/perception/native)
endif()
if(DESKWAIFU_BUILD_CONTROL)
  add_subdirectory(platform/control)
endif()
if(BUILD_TESTING)
  add_subdirectory(platform/tests)
endif()
deskwaifu_validate_boundaries()
if(BUILD_TESTING AND DESKWAIFU_PACKAGE_TESTS)
  include(cmake/InstallTests.cmake)
endif()
