file(MAKE_DIRECTORY "${FIXTURE_SOURCE}")
file(WRITE "${FIXTURE_SOURCE}/Transport.cpp" "#include \"LAppIPC.hpp\"\n")
execute_process(COMMAND "${CXX_COMPILER}" -std=c++17 "-I${APP_SOURCE}"
  -M "${FIXTURE_SOURCE}/Transport.cpp"
  RESULT_VARIABLE result OUTPUT_VARIABLE dependencies ERROR_VARIABLE error)
if (NOT result EQUAL 0)
  message(FATAL_ERROR "Transport header must compile independently: ${error}")
endif ()
if (dependencies MATCHES "AppCommandHandler|LAppDelegate|LAppLive2DManager")
  message(FATAL_ERROR "Transport header depends on concrete application components: ${dependencies}")
endif ()
if (NOT dependencies MATCHES "ICommandService.hpp")
  message(FATAL_ERROR "Transport must depend on the independent command port")
endif ()
