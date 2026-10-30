# Repository-wide script gates. Domain targets live with their owning plugins;
# the root test entry point is platform/tests.

if(Python3_Interpreter_FOUND)
  foreach(script smoke_isolation test_environment test_gate aur_packaging ocr_build release_packaging)
    string(REPLACE "_" "-" test_name ${script})
    add_test(NAME ${test_name} COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_${script}.py)
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 30)
  endforeach()
  if(TARGET deskwaifu-ocr)
    add_test(NAME ocr-native-models COMMAND ${CMAKE_COMMAND} -E env
      "OCR_NATIVE_HELPER=$<TARGET_FILE:deskwaifu-ocr>"
      ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_ocr_native_models.py)
    set_tests_properties(ocr-native-models PROPERTIES TIMEOUT 60)
  endif()
  foreach(script processor_boundaries plugin_boundaries qt_window_configuration ctl)
    string(REPLACE "_" "-" test_name ${script})
    add_test(NAME ${test_name} COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_${script}.py)
  endforeach()
  add_test(NAME build-boundaries COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_build_boundaries.py)
  set_tests_properties(build-boundaries PROPERTIES TIMEOUT 60 LABELS "architecture;regression")
  add_test(NAME opencode-review-cli COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/.gitea/scripts/test_opencode_review_cli.py)
  add_test(NAME opencode-review-extraction COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/.gitea/scripts/test_extract_opencode_review.py)
endif()
add_test(NAME smoke-exit-status COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/SmokeExitStatusTest.sh ${CMAKE_SOURCE_DIR})
add_test(NAME ci-image-private-transfer COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/CiImagePrivateTransferTest.sh ${CMAKE_SOURCE_DIR})
if(CMAKE_READELF AND CMAKE_NM)
  add_test(NAME plugin-host-link COMMAND ${CMAKE_COMMAND}
    -DHOST=$<TARGET_FILE:deskwaifu> -DREADELF=${CMAKE_READELF} -DNM=${CMAKE_NM}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckPluginHostLink.cmake)
endif()
if(DESKWAIFU_BUILD_DESKTOP_PLUGIN)
add_test(NAME plugin-no-speech-configure COMMAND ${CMAKE_COMMAND}
  -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DBINARY_DIR=${CMAKE_BINARY_DIR}/no-speech-configure
  -DSDK_ROOT=${SDK_ROOT_PATH} -DGLEW_ROOT=${GLEW_PATH} -DGENERATOR=${CMAKE_GENERATOR}
  -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckNoSpeechConfiguration.cmake)
endif()
