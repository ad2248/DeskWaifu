find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

qt_add_executable(waifuland-control
  main.cpp
  MainWindow.cpp
  MainWindow.hpp
  MainWindow.ui
  WaifulandClient.cpp
  WaifulandClient.hpp
)

target_link_libraries(waifuland-control PRIVATE
  Qt6::Widgets
  Qt6::Network
)

set_target_properties(waifuland-control PROPERTIES
  AUTOUIC ON
  AUTOMOC ON
)
