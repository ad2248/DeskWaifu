find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

qt_add_executable(deskwaifu-control
  main.cpp
  MainWindow.cpp
  MainWindow.hpp
  MainWindow.ui
  DeskWaifuClient.cpp
  DeskWaifuClient.hpp
  HotkeyPanel.cpp
  HotkeyPanel.hpp
)

target_link_libraries(deskwaifu-control PRIVATE
  Qt6::Widgets
  Qt6::Network
)

set_target_properties(deskwaifu-control PROPERTIES
  AUTOUIC ON
  AUTOMOC ON
)
