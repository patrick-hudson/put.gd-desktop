# Test labels, the shared test include directory and CTest scheduling, applied
# once every test is registered (the end of the top-level CMakeLists.txt).

if(NOT BUILD_TESTING)
  return()
endif()

# Every test target can include tests/fixtures/Waits.h as "fixtures/Waits.h".
get_property(putgd_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(target IN LISTS putgd_targets)
  get_target_property(putgd_target_type ${target} TYPE)
  if(putgd_target_type MATCHES "^(EXECUTABLE|MODULE_LIBRARY|SHARED_LIBRARY|STATIC_LIBRARY|OBJECT_LIBRARY)$"
      AND target MATCHES "^(test|bench)_")
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  endif()
endforeach()

# Labels. A `unit` test runs synchronously in its own process: no event-loop
# waits, threads, child processes, sockets, buses or displays, so a failure is
# deterministic. CI never retries one. Every other test is `integration`: CI
# retries a failed one once and reports a pass on the retry in the job summary
# and the flaky-test tracker issue (tools/ci/ctest_retry.py). A new test is
# `integration` unless it is added here.
set(PUTGD_UNIT_TESTS
  archive_contract bar_placement_contract bar_placement_single_monitor_contract build_contract
  export_av_sync_contract geometry_contract gnome_image_decode history_contract ip_address_contract
  json_ref_lifetime_contract kde_keyboard_map kde_shortcut_normalization login_startup_contract
  pause_timeline_contract payload_contract portal_streams_contract poster_contract public_url
  record_identity_contract record_runtime_contract recording_model_contract release_link_contract
  update_appimage_install_child update_check_request update_feed_contract update_minisign_verify
  update_system_repository upload_naming_contract)

get_property(putgd_tests DIRECTORY PROPERTY TESTS)
foreach(test IN LISTS putgd_tests)
  if(test IN_LIST PUTGD_UNIT_TESTS)
    set_property(TEST ${test} APPEND PROPERTY LABELS unit)
  else()
    set_property(TEST ${test} APPEND PROPERTY LABELS integration)
  endif()
endforeach()

# CPU-bound tests share the runner's cores. With a resource spec such as
# tools/ci/ctest-resources.json, CTest runs only that many of them at once
# while mostly idle tests keep the remaining slots.
foreach(test collision_policy_integration edit_operations_integration editor_contract
    history_contract s3_delete_integration s3_integration s3_multipart_integration version_contract)
  if(TEST ${test})
    set_tests_properties(${test} PROPERTIES RESOURCE_GROUPS "cpu:1")
  endif()
endforeach()
