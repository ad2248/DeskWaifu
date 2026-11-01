# Use the real versioned plugin metadata, but intentionally omit its dependency's
# search path to exercise dlopen failure without affecting the test host startup.
add_library(ocr-test-dependency SHARED ${CMAKE_CURRENT_LIST_DIR}/OcrTestDependency.cpp)
set_target_properties(ocr-test-dependency PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/missing-dependency")
add_library(ocr-missing-dependency MODULE ${CMAKE_CURRENT_LIST_DIR}/OcrTestModule.cpp)
set_target_properties(ocr-missing-dependency PROPERTIES AUTOMOC ON PREFIX "" SKIP_BUILD_RPATH ON)
target_compile_definitions(ocr-missing-dependency PRIVATE OCR_TEST_MISSING_DEPENDENCY)
target_link_libraries(ocr-missing-dependency PRIVATE Qt6::Core ocr-test-dependency)
add_library(ocr-incompatible-module MODULE ${CMAKE_CURRENT_LIST_DIR}/OcrIncompatibleModule.cpp)
set_target_properties(ocr-incompatible-module PROPERTIES AUTOMOC ON PREFIX "")
target_link_libraries(ocr-incompatible-module PRIVATE Qt6::Core)
target_compile_definitions(ocr-client-test PRIVATE
  OCR_MISSING_DEPENDENCY_MODULE="$<TARGET_FILE:ocr-missing-dependency>"
  OCR_INCOMPATIBLE_MODULE="$<TARGET_FILE:ocr-incompatible-module>")
add_dependencies(ocr-client-test ocr-missing-dependency ocr-incompatible-module)
