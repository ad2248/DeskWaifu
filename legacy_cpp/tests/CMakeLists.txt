# Repository-wide script gates. Domain targets live with their owning plugins;
# the root test entry point is platform/tests.

if(Python3_Interpreter_FOUND)
  foreach(script smoke_isolation test_environment test_gate build_boundary_registration sdk_configuration aur_packaging aur_binary_packaging release_packaging installed_testing mutation_configuration)
    string(REPLACE "_" "-" test_name ${script})
    deskwaifu_add_test(NAME ${test_name} COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_${script}.py)
  endforeach()
  foreach(script processor_boundaries plugin_boundaries)
    string(REPLACE "_" "-" test_name ${script})
    deskwaifu_add_test(NAME ${test_name} COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_${script}.py)
  endforeach()
  include(BuildBoundaryTests.cmake)
endif()
deskwaifu_add_test(NAME ci-image-private-transfer COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/CiImagePrivateTransferTest.sh ${CMAKE_SOURCE_DIR})
