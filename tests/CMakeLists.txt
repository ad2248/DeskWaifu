find_package(Qt6 REQUIRED COMPONENTS Test Network)

qt_add_executable(waifuland-client-test
  WaifulandClientTest.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../control/WaifulandClient.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../control/WaifulandClient.hpp
)

target_include_directories(waifuland-client-test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../control
)

target_link_libraries(waifuland-client-test PRIVATE
  Qt6::Test
  Qt6::Network
)

set_target_properties(waifuland-client-test PROPERTIES AUTOMOC ON)
add_test(NAME waifuland-client COMMAND waifuland-client-test)
