# Repository-wide script gates. Domain targets live with their owning plugins;
# the root test entry point is platform/tests.

if(Python3_Interpreter_FOUND)
  foreach(script smoke_isolation test_environment test_gate build_boundary_registration sdk_configuration aur_packaging aur_binary_packaging release_packaging installed_testing mutation_configuration documentation_links)
    string(REPLACE "_" "-" test_name ${script})
    deskwaifu_add_test(NAME ${test_name} COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_${script}.py)
  endforeach()
  foreach(script processor_boundaries plugin_boundaries)
    string(REPLACE "_" "-" test_name ${script})
    deskwaifu_add_test(NAME ${test_name} COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_${script}.py)
  endforeach()
  include(BuildBoundaryTests.cmake)
  deskwaifu_add_test(NAME opencode-review-cli COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/.gitea/scripts/test_opencode_review_cli.py)
  deskwaifu_add_test(NAME opencode-review-extraction COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/.gitea/scripts/test_extract_opencode_review.py)
endif()
deskwaifu_add_test(NAME smoke-exit-status COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/SmokeExitStatusTest.sh ${CMAKE_SOURCE_DIR})
deskwaifu_add_test(NAME ci-image-private-transfer COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/CiImagePrivateTransferTest.sh ${CMAKE_SOURCE_DIR})
