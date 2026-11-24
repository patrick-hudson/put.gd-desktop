# The payload, history, operations, storage and CLI contract loops.
# Included inside if(BUILD_TESTING) in the top-level CMakeLists.txt.
  foreach(contract payload archive upload_naming history operations recording_model capture_delay)
    add_executable(test_${contract} tests/unit/test_${contract}.cpp)
    target_link_libraries(test_${contract} PRIVATE putgd_app Qt6::Test)
    target_include_directories(test_${contract} PRIVATE tests)
    add_test(NAME ${contract}_contract COMMAND test_${contract})
    set_tests_properties(${contract}_contract PROPERTIES TIMEOUT 60)
  endforeach()
  set_tests_properties(operations_contract PROPERTIES TIMEOUT 120)
  # Generic image previews use the production transient GUI worker even under
  # a Core-only test parent; this remains a widget/dependency contract.
  add_dependencies(test_payload putgd-ui)
  add_dependencies(test_operations putgd-ui)
  set_tests_properties(payload_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
  if(PUTGD_SANITIZER STREQUAL "thread")
    # The 10,000-row contract instruments SQLite and Qt as well as the app.
    set_tests_properties(history_contract PROPERTIES TIMEOUT 300)
  endif()
  foreach(contract storage_contract cli config_reload storage_locations)
    add_executable(test_${contract} tests/integration/test_${contract}.cpp)
    target_link_libraries(test_${contract} PRIVATE putgd_app Qt6::Test)
    target_include_directories(test_${contract} PRIVATE tests)
    add_test(NAME ${contract}_integration COMMAND test_${contract})
    set_tests_properties(${contract}_integration PROPERTIES TIMEOUT 60)
  endforeach()
  target_compile_definitions(test_cli PRIVATE PUTGD_CLI_PATH="$<TARGET_FILE:putgd-cli>" PUTGD_APP_PATH="$<TARGET_FILE:putgd>")
  add_dependencies(test_cli putgd-cli putgd)
  # Moves an earlier putgd/putgd profile, interrupted at every rename, and
  # starts the real resident on upgraded and retained layouts (#56).
  target_compile_definitions(test_storage_locations PRIVATE PUTGD_CLI_PATH="$<TARGET_FILE:putgd-cli>" PUTGD_APP_PATH="$<TARGET_FILE:putgd>")
  add_dependencies(test_storage_locations putgd-cli putgd)
  set_tests_properties(storage_locations_integration PROPERTIES TIMEOUT 180)
