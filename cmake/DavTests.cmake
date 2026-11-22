# Nextcloud and WebDAV destination tests (#1872): the address and settings rules, and the
# production provider and worker against real servers named by environment variables
# (tests/integration/test_dav.cpp); without them each server case is skipped.
if(BUILD_TESTING)
  add_executable(test_dav_rules tests/unit/test_dav_rules.cpp)
  target_link_libraries(test_dav_rules PRIVATE putgd_dav_rules putgd_core Qt6::Test)
  add_test(NAME dav_rules COMMAND test_dav_rules)
  add_executable(test_dav tests/integration/test_dav.cpp)
  target_include_directories(test_dav PRIVATE tests)
  target_link_libraries(test_dav PRIVATE putgd_dav putgd_app Qt6::Test Qt6::Network)
  add_dependencies(test_dav putgd-webdav-worker)
  add_test(NAME dav_integration COMMAND test_dav)
  set_tests_properties(dav_integration PROPERTIES TIMEOUT 600)
  if(TARGET putgd-ui-catalog)
    target_link_libraries(putgd-ui-catalog PRIVATE putgd_dav_rules)
  endif()
endif()
