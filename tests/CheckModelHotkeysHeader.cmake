file(MAKE_DIRECTORY "${FIXTURE_SOURCE}")
file(WRITE "${FIXTURE_SOURCE}/binding.cpp"
  "#include \"processor/model/ModelHotkeys.hpp\"\nint main() { ModelHotkeys::Binding binding; return binding.expression + 1; }\n")
execute_process(COMMAND "${CXX_COMPILER}" -std=c++14 -fsyntax-only
  "-I${APP_SOURCE}" "${FIXTURE_SOURCE}/binding.cpp"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if (NOT result EQUAL 0)
  message(FATAL_ERROR "Binding requires SDK headers or is not self-contained: ${output}${error}")
endif ()
