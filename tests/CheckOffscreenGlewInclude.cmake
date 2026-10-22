# System-installed GLEW must not hide a missing bundled include dependency.
foreach(target deskwaifu_offscreen offscreen-renderer-test gpu-transfer-test)
  file(STRINGS "${INCLUDE_DIR}/${target}-includes.txt" includes)
  list(FIND includes "${GLEW_INCLUDE}" index)
  if(index EQUAL -1)
    message(FATAL_ERROR "${target} does not inherit the bundled GLEW include directory: ${GLEW_INCLUDE}")
  endif()
endforeach()
