cmake_minimum_required(VERSION 3.16)

# 设置应用名称。
set(APP_NAME deskwaifu)

# 根据当前布局设置目录路径。
set(SDK_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR}/CubismSdkForNative-5-r.5 CACHE PATH "Cubism SDK directory")
set(CORE_PATH ${SDK_ROOT_PATH}/Core)
set(FRAMEWORK_PATH ${SDK_ROOT_PATH}/Framework)
set(THIRD_PARTY_PATH ${CMAKE_CURRENT_SOURCE_DIR}/thirdParty)

set(STB_PATH ${THIRD_PARTY_PATH}/stb)
set(GLEW_PATH ${THIRD_PARTY_PATH}/glew CACHE PATH "GLEW source directory")

# 使用 SDK 中的着色器。
set(SAMPLE_SHADER_PATH ${SDK_ROOT_PATH}/Samples/OpenGL/Shaders/Standard)
set(FRAMEWORK_SHADER_PATH ${FRAMEWORK_PATH}/src/Rendering/OpenGL/Shaders/Standard)
set(COMMON_SRC_PATH ${SDK_ROOT_PATH}/Samples/Common)

# 设置项目。
project(${APP_NAME})
include(GNUInstallDirs)

include(CTest)
option(DESKWAIFU_BUILD_CONTROL "Build the Qt control application" ON)

# 设置输出目录。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)
# 设置构建配置，仅支持 Release 和 Debug。
set(CMAKE_CONFIGURATION_TYPES Debug Release
  CACHE STRING "Configurations" FORCE
)
# 禁止生成 ZERO_CHECK 项目。
set(CMAKE_SUPPRESS_REGENERATION ON)

# 禁用 GLEW 的相关构建步骤。
set(GLEW_EGL ON CACHE BOOL "" FORCE)
set(BUILD_UTILS OFF CACHE BOOL "" FORCE)

# 指定编译器标准版本。
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 添加 Cubism Core。
add_library(Live2DCubismCore STATIC IMPORTED)
set_target_properties(Live2DCubismCore
  PROPERTIES
    IMPORTED_LOCATION ${CORE_PATH}/lib/linux/x86_64/libLive2DCubismCore.a
    INTERFACE_INCLUDE_DIRECTORIES ${CORE_PATH}/include
)

# 查找 OpenGL 库。
find_package(OpenGL REQUIRED COMPONENTS OpenGL EGL)

# 添加 GLEW。
set(GLEW_EGL ON CACHE BOOL "" FORCE)
add_subdirectory(${GLEW_PATH}/build/cmake ${CMAKE_CURRENT_BINARY_DIR}/glew)

# 指定 Cubism Framework 的渲染后端。
set(FRAMEWORK_SOURCE OpenGL)
# 添加 Cubism Native Framework。
add_subdirectory(${FRAMEWORK_PATH} ${CMAKE_CURRENT_BINARY_DIR}/Framework)
# 为框架添加渲染宏定义。
target_compile_definitions(Framework PUBLIC CSM_TARGET_LINUX_GL)
# 为框架添加 GLEW 头文件路径。
target_include_directories(Framework PUBLIC ${GLEW_PATH}/include)
# 为框架链接依赖库。
target_link_libraries(Framework Live2DCubismCore glew_s)

# 合成器适配器使用的系统 D-Bus 依赖。
find_package(PkgConfig REQUIRED)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated)

# 创建应用可执行文件。
add_executable(${APP_NAME})
# 添加 SDK 通用源文件。
add_subdirectory(${COMMON_SRC_PATH} ${CMAKE_CURRENT_BINARY_DIR}/src/Common)
# 添加 ./src 中的项目源文件。
add_subdirectory(src)

install(TARGETS ${APP_NAME}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DeskWaifu)
install(PROGRAMS deskwaifu-ctl
  DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DeskWaifu)

# 为应用链接依赖库。
target_link_libraries(${APP_NAME}
  Framework
  ${OPENGL_LIBRARIES}
)
# 指定头文件目录。
target_include_directories(${APP_NAME} PRIVATE 
  ${STB_PATH}
)

# 配置阶段生成内嵌着色器头文件。
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
