# 每个 unittest 方法独立调度，保留方法内的完整参数矩阵与原有超时。
foreach(case IN ITEMS
    c_include_ownership
    compilation_ownership
    cpp_only_fixture_does_not_probe_unused_c_compiler
    forwarded_static_is_scoped_not_global
    include_ownership
    library_search_respects_static_dynamic_selection
    link_artifacts_do_not_depend_on_output_directory_or_argument_spelling
    link_ownership
    precompiled_headers_cannot_be_reused_across_owners
    renamed_vendor_directory_has_consistent_ownership
    same_owner_archives_and_existing_launchers_are_allowed
    sdk_include_surface_does_not_expose_plugin_private_headers
    static_archives_cannot_absorb_foreign_objects
    valid_same_owner_and_sdk_consumer_builds)
  string(REPLACE "_" "-" test_name "build-boundaries-${case}")
  deskwaifu_add_test(NAME "${test_name}" COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_CURRENT_LIST_DIR}/test_build_boundaries.py" "BuildBoundaries.test_${case}" -v)
  set_tests_properties("${test_name}" PROPERTIES TIMEOUT 60 LABELS "architecture;regression")
endforeach()
