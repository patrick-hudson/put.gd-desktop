# SFTP destination tests (#1871): the address, folder and link rules and the bounded reply
# parser; and an opt-in run of the production provider and worker against a real OpenSSH
# server, the storage lab's (tests/integration/test_sftp_live.cpp).
if(BUILD_TESTING)
  add_executable(test_sftp_rules tests/unit/test_sftp_rules.cpp)
  target_link_libraries(test_sftp_rules PRIVATE putgd_sftp_rules putgd_sftp_wire putgd_core Qt6::Test)
  add_test(NAME sftp_rules COMMAND test_sftp_rules)
  # #1890: storing one SFTP secret keeps the other when the keychain entry is locked.
  add_executable(test_sftp_store_secrets tests/unit/test_sftp_store_secrets.cpp)
  target_include_directories(test_sftp_store_secrets PRIVATE tests)
  target_link_libraries(test_sftp_store_secrets PRIVATE putgd_app putgd_sftp_rules putgd_core Qt6::Test)
  add_test(NAME sftp_store_secrets COMMAND test_sftp_store_secrets)
  add_executable(test_sftp_live EXCLUDE_FROM_ALL tests/integration/test_sftp_live.cpp)
  target_link_libraries(test_sftp_live PRIVATE putgd_sftp putgd_core Qt6::Test)
  add_dependencies(test_sftp_live putgd-sftp-worker putgd-sftp-askpass)
  foreach(area IN LISTS PUTGD_SETTINGS_GUI_AREAS)
    target_link_libraries(test_gui_settings_${area} PRIVATE putgd_sftp)
  endforeach()
endif()
