cmake_minimum_required(VERSION 3.25)

# 系统安装的 GLEW 不能掩盖内置头文件依赖缺失的问题。
foreach(target deskwaifu_offscreen offscreen-renderer-test gpu-transfer-test)
  file(STRINGS "${INCLUDE_DIR}/${target}-includes.txt" includes)
  list(FIND includes "${GLEW_INCLUDE}" index)
  if(index EQUAL -1)
    message(FATAL_ERROR "${target} does not inherit the bundled GLEW include directory: ${GLEW_INCLUDE}")
  endif()
endforeach()
