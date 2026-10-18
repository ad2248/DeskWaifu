cmake_minimum_required(VERSION 3.16)

# Set app name.
set(APP_NAME deskwaifu)

# Set directory paths based on the new layout
set(SDK_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR}/CubismSdkForNative-5-r.5 CACHE PATH "Cubism SDK directory")
set(CORE_PATH ${SDK_ROOT_PATH}/Core)
set(FRAMEWORK_PATH ${SDK_ROOT_PATH}/Framework)
set(THIRD_PARTY_PATH ${CMAKE_CURRENT_SOURCE_DIR}/thirdParty)

set(STB_PATH ${THIRD_PARTY_PATH}/stb)
set(GLEW_PATH ${THIRD_PARTY_PATH}/glew CACHE PATH "GLEW source directory")

# Shaders from SDK
set(SAMPLE_SHADER_PATH ${SDK_ROOT_PATH}/Samples/OpenGL/Shaders/Standard)
set(FRAMEWORK_SHADER_PATH ${FRAMEWORK_PATH}/src/Rendering/OpenGL/Shaders/Standard)
set(COMMON_SRC_PATH ${SDK_ROOT_PATH}/Samples/Common)

# Set project.
project(${APP_NAME})
include(GNUInstallDirs)

include(CTest)
option(DESKWAIFU_BUILD_CONTROL "Build the Qt control application" ON)

# Define output directory.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)
# Set configuration (Release and Debug only).
set(CMAKE_CONFIGURATION_TYPES Debug Release
  CACHE STRING "Configurations" FORCE
)
# Suppress generation of ZERO_CHECK project.
set(CMAKE_SUPPRESS_REGENERATION ON)

# Suppress GLEW process.
set(GLEW_EGL ON CACHE BOOL "" FORCE)
set(BUILD_UTILS OFF CACHE BOOL "" FORCE)

# Specify version of compiler.
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Add Cubism Core.
add_library(Live2DCubismCore STATIC IMPORTED)
set_target_properties(Live2DCubismCore
  PROPERTIES
    IMPORTED_LOCATION ${CORE_PATH}/lib/linux/x86_64/libLive2DCubismCore.a
    INTERFACE_INCLUDE_DIRECTORIES ${CORE_PATH}/include
)

# Find opengl libraries.
find_package(OpenGL REQUIRED COMPONENTS OpenGL EGL)

# Add GLEW.
set(GLEW_EGL ON CACHE BOOL "" FORCE)
add_subdirectory(${GLEW_PATH}/build/cmake ${CMAKE_CURRENT_BINARY_DIR}/glew)

# Specify Cubism Framework rendering.
set(FRAMEWORK_SOURCE OpenGL)
# Add Cubism Native Framework.
add_subdirectory(${FRAMEWORK_PATH} ${CMAKE_CURRENT_BINARY_DIR}/Framework)
# Add rendering definition to framework.
target_compile_definitions(Framework PUBLIC CSM_TARGET_LINUX_GL)
# Add include path of GLEW to framework.
target_include_directories(Framework PUBLIC ${GLEW_PATH}/include)
# Link libraries to framework.
target_link_libraries(Framework Live2DCubismCore glew_s)

# System D-Bus dependency used by the compositor adapter.
find_package(PkgConfig REQUIRED)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated)

# Make executable app.
add_executable(${APP_NAME})
# Add common source files from SDK.
add_subdirectory(${COMMON_SRC_PATH} ${CMAKE_CURRENT_BINARY_DIR}/src/Common)
# Add our source files in ./src
add_subdirectory(src)

install(TARGETS ${APP_NAME}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DeskWaifu)
install(PROGRAMS deskwaifu-ctl
  DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DeskWaifu)

# Link libraries to app.
target_link_libraries(${APP_NAME}
  Framework
  ${OPENGL_LIBRARIES}
)
# Specify include directories.
target_include_directories(${APP_NAME} PRIVATE 
  ${STB_PATH}
)

# Generate embedded shaders header at configure time.
set(EMBEDDED_SHADERS_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedShaders.hpp)
execute_process(
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_shaders.sh
    ${FRAMEWORK_SHADER_PATH}
    ${SAMPLE_SHADER_PATH}
    ${EMBEDDED_SHADERS_HEADER}
)
target_include_directories(${APP_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

if (DESKWAIFU_BUILD_CONTROL)
  add_subdirectory(control)
endif ()

if (BUILD_TESTING)
  add_subdirectory(tests)
endif ()
