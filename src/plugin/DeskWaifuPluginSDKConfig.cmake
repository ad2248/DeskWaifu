include(CMakeFindDependencyMacro)
find_dependency(Qt6 6.5 COMPONENTS Core)
include("${CMAKE_CURRENT_LIST_DIR}/DeskWaifuPluginSDKTargets.cmake")
