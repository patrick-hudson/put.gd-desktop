# Settings, logging, crash handler and diagnostics tests.
# Included inside if(BUILD_TESTING) in the top-level CMakeLists.txt.
  add_executable(test_settings tests/unit/test_settings.cpp)
  target_include_directories(test_settings PRIVATE src)
  target_link_libraries(test_settings PRIVATE putgd_core Qt6::Test)
  add_test(NAME settings_contract COMMAND test_settings)
  # Persistent logging, retention, redaction canaries and unclean exits (#74).
  add_executable(test_logging tests/unit/test_logging.cpp)
  target_include_directories(test_logging PRIVATE src tests)
  target_link_libraries(test_logging PRIVATE putgd_app putgd_s3_runtime Qt6::Test)
  add_test(NAME logging_contract COMMAND test_logging)
  set_tests_properties(logging_contract PROPERTIES TIMEOUT 120)
  # Diagnostics packages: contents, redaction, size limit and the default folder (#74 §3).
  if(TARGET putgd_crash)
    add_library(test_crash_module MODULE tests/fixtures/crash_module.cpp)
    # Linked, so its constructor starts a thread before the executable's own run (#211).
    add_library(test_crash_early_library SHARED tests/fixtures/crash_early_library.cpp)
    target_link_libraries(test_crash_early_library PRIVATE Threads::Threads)
    add_executable(test_crash_handler tests/unit/test_crash_handler.cpp)
    target_include_directories(test_crash_handler PRIVATE src tests)
    target_link_libraries(test_crash_handler PRIVATE putgd_crash putgd_log test_crash_early_library Qt6::Test ${CMAKE_DL_LIBS})
    target_compile_definitions(test_crash_handler PRIVATE PUTGD_CRASH_MODULE="$<TARGET_FILE:test_crash_module>"
                               PUTGD_CRASH_EARLY_LIBRARY="$<TARGET_FILE:test_crash_early_library>")
    add_dependencies(test_crash_handler test_crash_module)
    add_test(NAME crash_handler_contract COMMAND test_crash_handler)
  endif()
  add_executable(test_diagnostics_upload tests/unit/test_diagnostics_upload.cpp)
  target_include_directories(test_diagnostics_upload PRIVATE src tests)
  target_link_libraries(test_diagnostics_upload PRIVATE putgd_app Qt6::Test)
  add_test(NAME diagnostics_upload_contract COMMAND test_diagnostics_upload)
  add_executable(test_diagnostics_bundle tests/unit/test_diagnostics_bundle.cpp)
  target_include_directories(test_diagnostics_bundle PRIVATE src tests)
  target_link_libraries(test_diagnostics_bundle PRIVATE putgd_app Qt6::Network Qt6::Test)
  target_compile_definitions(test_diagnostics_bundle PRIVATE PUTGD_CLI_PATH="$<TARGET_FILE:putgd-cli>")
  add_dependencies(test_diagnostics_bundle putgd-cli)
  add_test(NAME diagnostics_bundle_contract COMMAND test_diagnostics_bundle)
  set_tests_properties(diagnostics_bundle_contract PROPERTIES TIMEOUT 180)
  set_tests_properties(settings_contract PROPERTIES TIMEOUT 30)
  add_test(NAME json_ref_lifetime_contract COMMAND "${CMAKE_COMMAND}"
    "-DREPO_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/json_ref_lifetime_contract.cmake")
  set_tests_properties(json_ref_lifetime_contract PROPERTIES TIMEOUT 30)
