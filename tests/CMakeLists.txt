find_package(Qt6 REQUIRED COMPONENTS Test Network)

qt_add_executable(deskwaifu-client-test
  DeskWaifuClientTest.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../control/DeskWaifuClient.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../control/DeskWaifuClient.hpp
)

target_include_directories(deskwaifu-client-test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../control
)

target_link_libraries(deskwaifu-client-test PRIVATE
  Qt6::Test
  Qt6::Network
)

set_target_properties(deskwaifu-client-test PROPERTIES AUTOMOC ON)
add_test(NAME deskwaifu-client COMMAND deskwaifu-client-test)
